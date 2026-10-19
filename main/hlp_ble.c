#include "hlp_ble.h"
#include "hlp_protocol.h"
#include "hud_state.h"
#include "connection_diag.h"
#include "host/ble_att.h"
#include "host/ble_hs.h"
#include "host/ble_uuid.h"
#include "nimble/ble.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs_id.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include <string.h>

#define HLP_MAX_FRAME 512
#define ADV_RETRY_INITIAL_MS 1000
#define ADV_RETRY_MAX_MS 60000
#define NOTIFY_SUBSCRIBE_TIMEOUT_MS 8000
static const char *TAG = "hlp_ble";
static const ble_uuid128_t SERVICE = BLE_UUID128_INIT(0x01,0x00,0x4c,0x50,0x4c,0x48,0x9d,0x9a,0x48,0x4c,0x6e,0x4d,0x01,0x00,0x7e,0x8a);
static const ble_uuid128_t TX = BLE_UUID128_INIT(0x01,0x00,0x4c,0x50,0x4c,0x48,0x9d,0x9a,0x48,0x4c,0x6e,0x4d,0x02,0x00,0x7e,0x8a);
static const ble_uuid128_t RX = BLE_UUID128_INIT(0x01,0x00,0x4c,0x50,0x4c,0x48,0x9d,0x9a,0x48,0x4c,0x6e,0x4d,0x03,0x00,0x7e,0x8a);
static const ble_uuid128_t CAPS = BLE_UUID128_INIT(0x01,0x00,0x4c,0x50,0x4c,0x48,0x9d,0x9a,0x48,0x4c,0x6e,0x4d,0x04,0x00,0x7e,0x8a);
typedef struct {
    uint16_t length;
    uint32_t queued_at_ms;
    uint8_t bytes[HLP_MAX_FRAME];
} chunk_t;
static QueueHandle_t queue;
static TaskHandle_t host_task_handle;
static volatile uint16_t conn = BLE_HS_CONN_HANDLE_NONE;
static uint16_t rx_handle;
static volatile bool subscribed;
static volatile bool announce_device;
static volatile bool host_synced;
static volatile uint32_t connected_at_ms;
static uint8_t address_type;
static struct ble_gap_adv_params adv_params;
static uint32_t advertising_retry_at_ms;
static uint32_t advertising_retry_delay_ms = ADV_RETRY_INITIAL_MS;

static void advertise(void);

static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static void schedule_advertising_retry(int rc)
{
    advertising_retry_at_ms = now_ms() + advertising_retry_delay_ms;
    ESP_LOGW(TAG, "Advertising retry scheduled in %u ms (error=%d)",
             (unsigned)advertising_retry_delay_ms, rc);
    advertising_retry_delay_ms *= 2;
    if (advertising_retry_delay_ms > ADV_RETRY_MAX_MS)
        advertising_retry_delay_ms = ADV_RETRY_MAX_MS;
}

static const char *disconnect_reason_name(int reason)
{
    switch (reason) {
    case BLE_HS_HCI_ERR(BLE_ERR_REM_USER_CONN_TERM):
        return "remote user terminated";
    case BLE_HS_HCI_ERR(BLE_ERR_RD_CONN_TERM_RESRCS):
        return "remote device low resources";
    case BLE_HS_HCI_ERR(BLE_ERR_RD_CONN_TERM_PWROFF):
        return "remote device powered off";
    case BLE_HS_HCI_ERR(BLE_ERR_CONN_TERM_LOCAL):
        return "local host terminated";
    case BLE_HS_HCI_ERR(BLE_ERR_CONN_SPVN_TMO):
        return "supervision timeout";
    default:
        return "other";
    }
}

static void log_connection_params(uint16_t handle, int update_status)
{
    struct ble_gap_conn_desc desc;
    const int rc = ble_gap_conn_find(handle, &desc);
    if (rc != 0) {
        ESP_LOGW(TAG, "Could not read BLE connection parameters (handle=%u rc=%d)",
                 handle, rc);
        return;
    }

    ESP_LOGI(TAG,
             "BLE link params: handle=%u update_status=%d interval=%.2fms latency=%u timeout=%ums",
             handle, update_status, desc.conn_itvl * 1.25, desc.conn_latency,
             desc.supervision_timeout * 10);
}

void hlp_protocol_send_line(const char *line) {
    if (!subscribed || conn == BLE_HS_CONN_HANDLE_NONE) return;
    char out[HLP_MAX_FRAME]; size_t n = strlen(line);
    if (n + 1 > sizeof(out)) return;
    memcpy(out, line, n); out[n++] = '\n';
    size_t mtu = ble_att_mtu(conn), payload = mtu > 3 ? mtu - 3 : 20;
    for (size_t off = 0; off < n; off += payload) {
        size_t part = n - off; if (part > payload) part = payload;
        int rc = BLE_HS_ENOMEM;
        for (int attempt = 0; attempt < 3 && rc == BLE_HS_ENOMEM; ++attempt) {
            struct os_mbuf *om = ble_hs_mbuf_from_flat(out + off, part);
            rc = om ? ble_gatts_notify_custom(conn, rx_handle, om) : BLE_HS_ENOMEM;
            if (rc == BLE_HS_ENOMEM) vTaskDelay(pdMS_TO_TICKS(5));
        }
        if (rc != 0) {
            ESP_LOGW(TAG, "HLP notification failed: %d", rc);
            connection_diag_event(CONN_DIAG_GATT_ERROR, rc, (int32_t)part);
            return;
        }
    }
}

static int access_cb(uint16_t ch, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg) {
    (void)ch; (void)attr; (void)arg;
    if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
        const uint64_t callback_start_us = esp_timer_get_time();
        size_t n = OS_MBUF_PKTLEN(ctxt->om);
        if (!n || n > HLP_MAX_FRAME) {
            connection_diag_note_rx((uint32_t)n,
                                    (uint8_t)uxQueueMessagesWaiting(queue),
                                    (uint32_t)(esp_timer_get_time() - callback_start_us),
                                    false);
            return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
        }
        chunk_t item = { .length = n };
        if (os_mbuf_copydata(ctxt->om, 0, n, item.bytes) != 0) {
            connection_diag_note_rx((uint32_t)n,
                                    (uint8_t)uxQueueMessagesWaiting(queue),
                                    (uint32_t)(esp_timer_get_time() - callback_start_us),
                                    false);
            return BLE_ATT_ERR_UNLIKELY;
        }
        item.queued_at_ms = now_ms();
        if (xQueueSend(queue, &item, 0) == pdTRUE) {
            connection_diag_note_rx((uint32_t)n,
                                    (uint8_t)uxQueueMessagesWaiting(queue),
                                    (uint32_t)(esp_timer_get_time() - callback_start_us),
                                    true);
            return 0;
        }
        connection_diag_note_rx((uint32_t)n,
                                (uint8_t)uxQueueMessagesWaiting(queue),
                                (uint32_t)(esp_timer_get_time() - callback_start_us),
                                false);
        connection_diag_event(CONN_DIAG_RX_QUEUE_FULL, (int32_t)n,
                              (int32_t)uxQueueMessagesWaiting(queue));
        return BLE_ATT_ERR_INSUFFICIENT_RES;
    }
    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
        const char caps[] = "{\"v\":1,\"caps\":{\"transport\":\"ble\",\"maxFrame\":512}}\n";
        return os_mbuf_append(ctxt->om, caps, sizeof(caps)-1) == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
    }
    return BLE_ATT_ERR_UNLIKELY;
}
static struct ble_gatt_chr_def chars[] = {
    { .uuid=&TX.u, .access_cb=access_cb, .flags=BLE_GATT_CHR_F_WRITE },
    { .uuid=&RX.u, .access_cb=access_cb, .val_handle=&rx_handle, .flags=BLE_GATT_CHR_F_NOTIFY },
    { .uuid=&CAPS.u, .access_cb=access_cb, .flags=BLE_GATT_CHR_F_READ }, {0}
};
static const struct ble_gatt_svc_def services[] = {{ .type=BLE_GATT_SVC_TYPE_PRIMARY, .uuid=&SERVICE.u, .characteristics=chars }, {0}};

static int gap_event(struct ble_gap_event *e, void *arg) {
    (void)arg;
    switch (e->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (!e->connect.status) {
            conn = e->connect.conn_handle;
            connected_at_ms = now_ms();
            hud_state_set_ble_connected(true);
            hlp_protocol_reset_session();
            ESP_LOGI(TAG, "BLE connection established (handle=%u)", conn);
            connection_diag_set_ble_state(true, subscribed);
            connection_diag_event(CONN_DIAG_CONNECT, conn, 0);
            advertising_retry_at_ms = 0;
            log_connection_params(conn, 0);
        } else {
            conn = BLE_HS_CONN_HANDLE_NONE;
            connected_at_ms = 0;
            ESP_LOGW(TAG, "BLE connection attempt failed: %d", e->connect.status);
            connection_diag_set_ble_state(false, false);
            connection_diag_event(CONN_DIAG_CONNECT_FAIL, e->connect.status, 0);
            advertise();
        }
        break;
    case BLE_GAP_EVENT_DISCONNECT: {
        connection_diag_link_end(e->disconnect.reason, -127,
                                 (uint8_t)uxQueueMessagesWaiting(queue));
        conn = BLE_HS_CONN_HANDLE_NONE;
        subscribed = false;
        connected_at_ms = 0;
        hud_state_set_ble_connected(false);
        hlp_protocol_reset_session();
        ESP_LOGI(TAG, "BLE disconnected (reason=%d, %s); restarting advertising",
                 e->disconnect.reason, disconnect_reason_name(e->disconnect.reason));
        connection_diag_set_ble_state(false, false);
        advertise();
        break;
    }
    case BLE_GAP_EVENT_SUBSCRIBE:
        ESP_LOGI(TAG,
                 "BLE CCCD event: conn=%u attr=%u RX_attr=%u reason=%u notify=%u->%u indicate=%u->%u",
                 e->subscribe.conn_handle, e->subscribe.attr_handle, rx_handle,
                 e->subscribe.reason, e->subscribe.prev_notify, e->subscribe.cur_notify,
                 e->subscribe.prev_indicate, e->subscribe.cur_indicate);
        if (e->subscribe.attr_handle == rx_handle) {
            subscribed = e->subscribe.cur_notify;
            ESP_LOGI(TAG, "BLE notifications %s", subscribed ? "enabled" : "disabled");
            connection_diag_set_ble_state(conn != BLE_HS_CONN_HANDLE_NONE, subscribed);
            connection_diag_event(subscribed ? CONN_DIAG_SUBSCRIBE : CONN_DIAG_UNSUBSCRIBE,
                                  e->subscribe.attr_handle, 0);
            if (subscribed) announce_device = true;
        } else {
            connection_diag_event(CONN_DIAG_NO_NOTIFY, e->subscribe.attr_handle,
                                  (int32_t)rx_handle);
        }
        break;
    case BLE_GAP_EVENT_CONN_UPDATE:
        ESP_LOGI(TAG, "BLE connection parameter update completed: status=%d",
                 e->conn_update.status);
        connection_diag_event(CONN_DIAG_CONN_UPDATE, e->conn_update.status,
                              e->conn_update.conn_handle);
        log_connection_params(e->conn_update.conn_handle, e->conn_update.status);
        break;
    case BLE_GAP_EVENT_ADV_COMPLETE:
        ESP_LOGW(TAG, "BLE advertising completed unexpectedly: reason=%d",
                 e->adv_complete.reason);
        connection_diag_event(CONN_DIAG_ADV_COMPLETE, e->adv_complete.reason, 0);
        if (conn == BLE_HS_CONN_HANDLE_NONE) advertise();
        break;
    default: break;
    }
    return 0;
}

static int start_advertising(void) {
    int rc = ble_gap_adv_start(address_type, NULL, BLE_HS_FOREVER, &adv_params, gap_event, NULL);
    if (rc) {
        ESP_LOGE(TAG, "BLE advertising start failed: %d", rc);
        connection_diag_event(CONN_DIAG_ADV_FAIL, rc, 0);
        schedule_advertising_retry(rc);
    } else {
        ESP_LOGI(TAG, "BLE advertising started as WazeHUD (address_type=%u, connectable)", address_type);
        connection_diag_event(CONN_DIAG_ADV_START, address_type, 0);
        advertising_retry_at_ms = 0;
        advertising_retry_delay_ms = ADV_RETRY_INITIAL_MS;
    }
    return rc;
}

static void advertise(void) {
    struct ble_hs_adv_fields f = {0};
    f.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    f.name = (uint8_t *)"WazeHUD"; f.name_len = 7; f.name_is_complete = 1;
    f.uuids128 = (ble_uuid128_t *)&SERVICE; f.num_uuids128 = 1; f.uuids128_is_complete = 1;
    int rc = ble_gap_adv_set_fields(&f);
    if (rc) {
        ESP_LOGE(TAG, "BLE advertising data setup failed: %d", rc);
        connection_diag_event(CONN_DIAG_ADV_FAIL, rc, 1);
        schedule_advertising_retry(rc);
        return;
    }
    adv_params = (struct ble_gap_adv_params){0};
    adv_params.conn_mode = BLE_GAP_CONN_MODE_UND;
    adv_params.disc_mode = BLE_GAP_DISC_MODE_GEN;
    adv_params.itvl_min = BLE_GAP_ADV_FAST_INTERVAL1_MIN;
    adv_params.itvl_max = BLE_GAP_ADV_FAST_INTERVAL1_MAX;
    adv_params.channel_map = BLE_GAP_ADV_DFLT_CHANNEL_MAP;
    start_advertising();
}

static void synced(void) {
    int rc = ble_hs_id_infer_auto(0, &address_type);
    if (rc) {
        ESP_LOGE(TAG, "BLE host sync failed to infer address: %d", rc);
        return;
    }
    host_synced = true;
    ESP_LOGI(TAG, "BLE host synchronized (address_type=%u)", address_type);
    connection_diag_event(CONN_DIAG_BLE_SYNC, address_type, 0);
    advertise();
}

static void host_task(void *arg) {
    (void)arg;
    host_task_handle = xTaskGetCurrentTaskHandle();
    ESP_LOGI(TAG, "NimBLE host task started");
    nimble_port_run();
    ESP_LOGW(TAG, "NimBLE host task stopped");
    connection_diag_event(CONN_DIAG_HOST_STOPPED, 0, 0);
    nimble_port_freertos_deinit();
}
static void protocol_task(void *arg) {
    (void)arg;
    chunk_t item;
    uint32_t last_health_ms = 0;
    uint32_t subscribed_since_ms = 0;
    bool observed_subscribed = false;
    bool data_was_stale = false;
    bool no_notify_logged = false;
    for (;;) {
        if (hlp_protocol_take_session_reset()) {
            observed_subscribed = false;
            data_was_stale = false;
            no_notify_logged = false;
        }
        if (announce_device) { announce_device=false; hlp_protocol_send_line("{\"v\":1,\"t\":\"dev\",\"name\":\"Waveshare HUD 4.3\",\"fw\":\"0.2.0\",\"proto\":[1],\"disp\":{\"w\":800,\"h\":480,\"color\":1},\"can\":[\"speed\",\"limit\",\"turn\",\"lanes\",\"avgzone\",\"street\",\"nextStreet\",\"time\",\"eta\",\"remaining\",\"progress\",\"alerts\"],\"want\":{\"rate\":4,\"fields\":[\"spd\",\"lim\",\"over\",\"trn\",\"trn2\",\"dst\",\"exit\",\"lan\",\"avg\",\"avgL\",\"avgR\",\"avgP\",\"st\",\"st2\",\"eta\",\"hour\",\"minute\",\"time\",\"rmin\",\"rm\",\"rkm\",\"totalM\",\"routeM\",\"tdst\",\"progress\",\"alr\",\"alrD\",\"alrV\",\"alrS\",\"alrM\",\"alrs\"]}}"); }
        if (xQueueReceive(queue, &item, pdMS_TO_TICKS(100)) == pdTRUE) {
            const uint32_t queue_wait_ms = now_ms() - item.queued_at_ms;
            const uint64_t parse_start_us = esp_timer_get_time();
            hlp_protocol_feed(item.bytes, item.length);
            const uint32_t parse_time_us = (uint32_t)(esp_timer_get_time() - parse_start_us);
            connection_diag_note_rx_processed(queue_wait_ms, parse_time_us);
        }

        const uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
        if (subscribed && !observed_subscribed) {
            observed_subscribed = true;
            subscribed_since_ms = now;
        } else if (!subscribed) {
            observed_subscribed = false;
            data_was_stale = false;
        }
        const bool link_active = conn != BLE_HS_CONN_HANDLE_NONE && subscribed;
        uint32_t status_age_ms = UINT32_MAX;
        if (link_active) {
            status_age_ms = hlp_protocol_status_age_ms(now);
            if (status_age_ms == UINT32_MAX) status_age_ms = now - subscribed_since_ms;
            const bool stale = status_age_ms >= 4000;
            if (stale != data_was_stale) {
                data_was_stale = stale;
                connection_diag_event(stale ? CONN_DIAG_DATA_STALE : CONN_DIAG_DATA_RESUMED,
                                      (int32_t)(status_age_ms / 1000), 0);
            }
        }
        if (conn != BLE_HS_CONN_HANDLE_NONE && !subscribed &&
            !no_notify_logged && connected_at_ms &&
            now - connected_at_ms >= NOTIFY_SUBSCRIBE_TIMEOUT_MS) {
            no_notify_logged = true;
            ESP_LOGW(TAG,
                     "BLE connected for %u ms without subscribing to RX notifications (conn=%u RX_attr=%u)",
                     (unsigned)(now - connected_at_ms), (unsigned)conn, rx_handle);
            connection_diag_event(CONN_DIAG_NO_NOTIFY, conn, rx_handle);
        }
        const uint32_t health_interval_ms =
            conn != BLE_HS_CONN_HANDLE_NONE ? 10000 : 60000;
        if (now - last_health_ms >= health_interval_ms) {
            last_health_ms = now;
            int8_t rssi = -127;
            if (conn != BLE_HS_CONN_HANDLE_NONE)
                ble_gap_conn_rssi(conn, &rssi);
            const uint16_t host_stack_hwm = host_task_handle
                ? (uint16_t)uxTaskGetStackHighWaterMark(host_task_handle) : 0;
            const uint16_t protocol_stack_hwm =
                (uint16_t)uxTaskGetStackHighWaterMark(NULL);
            connection_diag_health((uint8_t)uxQueueMessagesWaiting(queue),
                                   status_age_ms, rssi, host_stack_hwm,
                                   protocol_stack_hwm);
        }
        if (host_synced && conn == BLE_HS_CONN_HANDLE_NONE &&
            advertising_retry_at_ms &&
            (int32_t)(now - advertising_retry_at_ms) >= 0) {
            ESP_LOGW(TAG, "Retrying WazeHUD advertising");
            advertise();
        }
        connection_diag_process();
    }
}
void hlp_ble_start(void) {
    /* NVS is initialised once in app_main(). */
    ESP_LOGI(TAG, "Starting BLE peripheral");
    connection_diag_event(CONN_DIAG_BLE_START, 0, 0);
    queue = xQueueCreate(16, sizeof(chunk_t)); configASSERT(queue);
    ESP_ERROR_CHECK(nimble_port_init());
    ESP_LOGI(TAG, "NimBLE initialized");
    ble_svc_gap_init();
    ble_svc_gatt_init();
    int rc=ble_gatts_count_cfg(services); if (!rc) rc=ble_gatts_add_svcs(services); if (!rc) rc=ble_svc_gap_device_name_set("WazeHUD");
    if (rc) {
        ESP_LOGE(TAG, "GATT setup failed: %d", rc);
        connection_diag_event(CONN_DIAG_GATT_ERROR, rc, 0);
        return;
    }
    ESP_LOGI(TAG, "GATT service registered; device name set to WazeHUD");
    ble_hs_cfg.sync_cb=synced;
    if (xTaskCreate(protocol_task,"hlp_protocol",6144,NULL,5,NULL)!=pdPASS) { ESP_LOGE(TAG,"Protocol task creation failed"); return; }
    ESP_LOGI(TAG, "Starting NimBLE host task");
    TaskHandle_t host_task_handle = NULL;
    BaseType_t task_rc = xTaskCreatePinnedToCore(host_task, "nimble_host", NIMBLE_HS_STACK_SIZE,
                                                 NULL, configMAX_PRIORITIES - 4,
                                                 &host_task_handle, NIMBLE_CORE);
    if (task_rc != pdPASS) {
        size_t free_internal = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        size_t largest_internal = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        ESP_LOGE(TAG, "NimBLE host task creation failed: %ld (internal heap free=%u largest=%u)",
                 (long)task_rc, (unsigned)free_internal, (unsigned)largest_internal);
    }
}
