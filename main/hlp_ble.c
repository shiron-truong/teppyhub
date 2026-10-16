#include "hlp_ble.h"
#include "hlp_protocol.h"
#include "hud_state.h"
#include "host/ble_att.h"
#include "host/ble_hs.h"
#include "host/ble_uuid.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs_id.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include <string.h>

#define HLP_MAX_FRAME 512
static const char *TAG = "hlp_ble";
static const ble_uuid128_t SERVICE = BLE_UUID128_INIT(0x01,0x00,0x4c,0x50,0x4c,0x48,0x9d,0x9a,0x48,0x4c,0x6e,0x4d,0x01,0x00,0x7e,0x8a);
static const ble_uuid128_t TX = BLE_UUID128_INIT(0x01,0x00,0x4c,0x50,0x4c,0x48,0x9d,0x9a,0x48,0x4c,0x6e,0x4d,0x02,0x00,0x7e,0x8a);
static const ble_uuid128_t RX = BLE_UUID128_INIT(0x01,0x00,0x4c,0x50,0x4c,0x48,0x9d,0x9a,0x48,0x4c,0x6e,0x4d,0x03,0x00,0x7e,0x8a);
static const ble_uuid128_t CAPS = BLE_UUID128_INIT(0x01,0x00,0x4c,0x50,0x4c,0x48,0x9d,0x9a,0x48,0x4c,0x6e,0x4d,0x04,0x00,0x7e,0x8a);
typedef struct { uint16_t length; uint8_t bytes[HLP_MAX_FRAME]; } chunk_t;
static QueueHandle_t queue;
static uint16_t conn = BLE_HS_CONN_HANDLE_NONE, rx_handle;
static bool subscribed;
static volatile bool announce_device;
static uint8_t address_type;
static struct ble_gap_adv_params adv_params;

static int start_advertising(void);

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
        if (rc != 0) { ESP_LOGW(TAG, "HLP notification failed: %d", rc); return; }
    }
}

static int access_cb(uint16_t ch, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg) {
    (void)ch; (void)attr; (void)arg;
    if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
        size_t n = OS_MBUF_PKTLEN(ctxt->om);
        if (!n || n > HLP_MAX_FRAME) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
        chunk_t item = { .length = n };
        if (os_mbuf_copydata(ctxt->om, 0, n, item.bytes) != 0) return BLE_ATT_ERR_UNLIKELY;
        return xQueueSend(queue, &item, 0) == pdTRUE ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
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
            hud_state_set_ble_connected(true);
            ESP_LOGI(TAG, "BLE connection established (handle=%u)", conn);
        } else {
            conn = BLE_HS_CONN_HANDLE_NONE;
            ESP_LOGW(TAG, "BLE connection attempt failed: %d", e->connect.status);
            start_advertising();
        }
        break;
    case BLE_GAP_EVENT_DISCONNECT:
        conn = BLE_HS_CONN_HANDLE_NONE;
        subscribed = false;
        hud_state_set_ble_connected(false);
        ESP_LOGI(TAG, "BLE disconnected (reason=%d); restarting advertising", e->disconnect.reason);
        start_advertising();
        break;
    case BLE_GAP_EVENT_SUBSCRIBE:
        if (e->subscribe.attr_handle == rx_handle) {
            subscribed = e->subscribe.cur_notify;
            ESP_LOGI(TAG, "BLE notifications %s", subscribed ? "enabled" : "disabled");
            if (subscribed) announce_device = true;
        }
        break;
    default: break;
    }
    return 0;
}

static int start_advertising(void) {
    int rc = ble_gap_adv_start(address_type, NULL, BLE_HS_FOREVER, &adv_params, gap_event, NULL);
    if (rc) {
        ESP_LOGE(TAG, "BLE advertising start failed: %d", rc);
    } else {
        ESP_LOGI(TAG, "BLE advertising started as WazeHUD (address_type=%u, connectable)", address_type);
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
        return;
    }
    adv_params = (struct ble_gap_adv_params){0};
    adv_params.conn_mode = BLE_GAP_CONN_MODE_UND;
    adv_params.disc_mode = BLE_GAP_DISC_MODE_GEN;
    start_advertising();
}

static void synced(void) {
    int rc = ble_hs_id_infer_auto(0, &address_type);
    if (rc) {
        ESP_LOGE(TAG, "BLE host sync failed to infer address: %d", rc);
        return;
    }
    ESP_LOGI(TAG, "BLE host synchronized (address_type=%u)", address_type);
    advertise();
}

static void host_task(void *arg) {
    (void)arg;
    ESP_LOGI(TAG, "NimBLE host task started");
    nimble_port_run();
    ESP_LOGW(TAG, "NimBLE host task stopped");
    nimble_port_freertos_deinit();
}
static void protocol_task(void *arg) {
    (void)arg; chunk_t item;
    for (;;) {
        if (announce_device) { announce_device=false; hlp_protocol_send_line("{\"v\":1,\"t\":\"dev\",\"name\":\"Waveshare HUD 4.3\",\"fw\":\"0.2.0\",\"proto\":[1],\"disp\":{\"w\":800,\"h\":480,\"color\":1},\"can\":[\"speed\",\"limit\",\"turn\",\"lanes\",\"avgzone\",\"street\",\"nextStreet\",\"time\",\"eta\",\"remaining\",\"progress\",\"alerts\"],\"want\":{\"rate\":4,\"fields\":[\"spd\",\"lim\",\"over\",\"trn\",\"trn2\",\"dst\",\"exit\",\"lan\",\"avg\",\"avgL\",\"avgR\",\"avgP\",\"st\",\"st2\",\"eta\",\"hour\",\"minute\",\"time\",\"rmin\",\"rm\",\"rkm\",\"totalM\",\"routeM\",\"tdst\",\"progress\",\"alr\",\"alrD\",\"alrV\",\"alrS\",\"alrM\",\"alrs\"]}}"); }
        if (xQueueReceive(queue, &item, pdMS_TO_TICKS(100)) == pdTRUE) hlp_protocol_feed(item.bytes, item.length);
    }
}
void hlp_ble_start(void) {
    /* NVS is initialised once in app_main(). */
    ESP_LOGI(TAG, "Starting BLE peripheral");
    queue = xQueueCreate(16, sizeof(chunk_t)); configASSERT(queue);
    ESP_ERROR_CHECK(nimble_port_init());
    ESP_LOGI(TAG, "NimBLE initialized");
    ble_svc_gap_init();
    ble_svc_gatt_init();
    int rc=ble_gatts_count_cfg(services); if (!rc) rc=ble_gatts_add_svcs(services); if (!rc) rc=ble_svc_gap_device_name_set("WazeHUD");
    if (rc) { ESP_LOGE(TAG, "GATT setup failed: %d", rc); return; }
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
