#include "connection_diag.h"

#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/portmacro.h"
#include "freertos/queue.h"
#include "nvs.h"

#include <string.h>

#define DIAG_MAGIC 0x48444732u
#define DIAG_RECORDS 128
#define DIAG_IDLE_FLUSH_MS 5000

static const char *TAG = "conn_diag";
static const char *NVS_NAMESPACE = "ble_diag";
static const char *NVS_KEY = "records";

typedef struct {
    uint32_t boot_id;
    uint32_t uptime_ms;
    uint32_t uptime_s;
    uint32_t rx_writes;
    uint32_t rx_bytes;
    uint32_t rx_rejected;
    uint32_t status_frames;
    uint32_t status_age_s;
    uint32_t free_internal;
    uint32_t min_internal;
    uint32_t largest_internal;
    uint32_t free_psram;
    uint32_t largest_psram;
    uint32_t link_age_s;
    uint32_t rssi_age_s;
    uint32_t max_gatt_cb_us;
    uint32_t max_queue_wait_ms;
    uint32_t max_parse_us;
    uint16_t event;
    uint16_t speed_kmh;
    uint16_t speed_peak_kmh;
    uint16_t host_stack_hwm;
    uint16_t protocol_stack_hwm;
    uint8_t flags;
    uint8_t queue_depth;
    uint8_t queue_peak;
    int8_t rssi;
    uint16_t rx_max_bytes;
    int32_t arg0;
    int32_t arg1;
} record_t;

typedef struct {
    uint32_t magic;
    uint16_t count;
    uint16_t next;
    uint32_t boot_id;
    record_t records[DIAG_RECORDS];
} store_t;

typedef struct {
    connection_diag_event_t event;
    uint32_t uptime_ms;
    int32_t arg0;
    int32_t arg1;
    uint8_t queue_depth;
    uint32_t status_age_ms;
    uint32_t link_age_s;
    uint32_t rssi_age_s;
    uint32_t max_gatt_cb_us;
    uint32_t max_queue_wait_ms;
    uint32_t max_parse_us;
    uint16_t speed_kmh;
    uint16_t speed_peak_kmh;
    uint16_t host_stack_hwm;
    uint16_t protocol_stack_hwm;
    uint8_t queue_peak;
    int8_t rssi;
    uint8_t flags;
    uint32_t rx_writes;
    uint32_t rx_bytes;
    uint32_t rx_rejected;
    uint16_t rx_max_bytes;
    uint32_t status_frames;
    uint32_t free_internal;
    uint32_t min_internal;
    uint32_t largest_internal;
    uint32_t free_psram;
    uint32_t largest_psram;
} pending_event_t;

static store_t *store;
static QueueHandle_t event_queue;
static nvs_handle_t diag_nvs_handle;
static bool persistent;
static bool dirty;
static uint32_t disconnected_since_ms;
static uint32_t connected_since_ms;
static uint32_t last_status_ms;
static portMUX_TYPE stats_lock = portMUX_INITIALIZER_UNLOCKED;
static uint32_t rx_writes, rx_bytes, status_frames;
static uint32_t rx_rejected;
static uint16_t rx_max_bytes;
static uint32_t session_max_callback_us;
static uint32_t session_max_queue_wait_ms;
static uint32_t session_max_parse_us;
static uint8_t session_queue_peak;
static uint8_t link_flags;
static int16_t current_speed_kmh = -1;
static uint16_t speed_peak_kmh;
static int8_t last_rssi = -127;
static uint32_t last_rssi_ms;

static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static const char *event_name(uint16_t event)
{
    switch (event) {
    case CONN_DIAG_BOOT: return "BOOT";
    case CONN_DIAG_BLE_START: return "BLE_START";
    case CONN_DIAG_BLE_SYNC: return "BLE_SYNC";
    case CONN_DIAG_ADV_START: return "ADV_START";
    case CONN_DIAG_ADV_FAIL: return "ADV_FAIL";
    case CONN_DIAG_CONNECT: return "CONNECT";
    case CONN_DIAG_CONNECT_FAIL: return "CONNECT_FAIL";
    case CONN_DIAG_DISCONNECT: return "DISCONNECT";
    case CONN_DIAG_SUBSCRIBE: return "SUBSCRIBE";
    case CONN_DIAG_UNSUBSCRIBE: return "UNSUBSCRIBE";
    case CONN_DIAG_RX_QUEUE_FULL: return "RX_QUEUE_FULL";
    case CONN_DIAG_BAD_JSON: return "BAD_JSON";
    case CONN_DIAG_UNKNOWN_FRAME: return "UNKNOWN_FRAME";
    case CONN_DIAG_FRAME_OVERFLOW: return "FRAME_OVERFLOW";
    case CONN_DIAG_DATA_STALE: return "DATA_STALE";
    case CONN_DIAG_DATA_RESUMED: return "DATA_RESUMED";
    case CONN_DIAG_HEALTH: return "HEALTH";
    case CONN_DIAG_HOST_STOPPED: return "HOST_STOPPED";
    case CONN_DIAG_GATT_ERROR: return "GATT_ERROR";
    case CONN_DIAG_CONN_UPDATE: return "CONN_UPDATE";
    case CONN_DIAG_ADV_COMPLETE: return "ADV_COMPLETE";
    case CONN_DIAG_NO_NOTIFY: return "NO_NOTIFY";
    default: return "UNKNOWN";
    }
}

static void print_record(const record_t *r)
{
    ESP_LOGI(TAG,
             "boot=%u up=%us.%03us event=%s(%u) args=%ld,%ld link=%s%s link_age=%us "
             "speed=%d speed_peak=%u rssi=%d rssi_age_s=%ld rx=%u/%uB reject=%u "
             "rx_max=%uB status=%u age_s=%ld "
             "q=%u/q_peak=%u "
             "cb_max=%luus wait_max=%lums parse_max=%luus "
             "heap_int=%u/min=%u/largest=%u heap_psram=%u/largest=%u "
             "stack_hwm_host=%u/proto=%u",
             (unsigned)r->boot_id, (unsigned)r->uptime_s,
             (unsigned)(r->uptime_ms % 1000), event_name(r->event),
             (unsigned)r->event, (long)r->arg0, (long)r->arg1,
             (r->flags & 1) ? "connected" : "down",
             (r->flags & 2) ? "+notify" : "", (unsigned)r->link_age_s,
             r->speed_kmh == UINT16_MAX ? -1 : (int)r->speed_kmh,
             r->speed_peak_kmh,
             (int)r->rssi,
             r->rssi_age_s == UINT32_MAX ? -1L : (long)r->rssi_age_s,
             (unsigned)r->rx_writes, (unsigned)r->rx_bytes,
             (unsigned)r->rx_rejected, (unsigned)r->rx_max_bytes,
             (unsigned)r->status_frames,
             r->status_age_s == UINT32_MAX ? -1L : (long)r->status_age_s,
             (unsigned)r->queue_depth, (unsigned)r->queue_peak,
             (unsigned long)r->max_gatt_cb_us,
             (unsigned long)r->max_queue_wait_ms, (unsigned long)r->max_parse_us,
             (unsigned)r->free_internal, (unsigned)r->min_internal,
             (unsigned)r->largest_internal, (unsigned)r->free_psram,
             (unsigned)r->largest_psram, (unsigned)r->host_stack_hwm,
             (unsigned)r->protocol_stack_hwm);
}

static void persist_store(void)
{
    if (!persistent || !store) return;
    esp_err_t err = nvs_set_blob(diag_nvs_handle, NVS_KEY, store, sizeof(*store));
    if (err == ESP_OK) err = nvs_commit(diag_nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Could not persist diagnostics: %s; continuing with RAM log only",
                 esp_err_to_name(err));
        persistent = false;
    } else {
        dirty = false;
    }
}

void connection_diag_init(void)
{
    store = heap_caps_calloc(1, sizeof(*store), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!store) {
        ESP_LOGE(TAG, "Could not allocate diagnostic ring in PSRAM; persistent diagnostics disabled");
        persistent = false;
        return;
    }
    store->magic = DIAG_MAGIC;

    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &diag_nvs_handle);
    if (err == ESP_OK) {
        size_t size = 0;
        err = nvs_get_blob(diag_nvs_handle, NVS_KEY, NULL, &size);
        if (err == ESP_OK && size == sizeof(*store)) {
            err = nvs_get_blob(diag_nvs_handle, NVS_KEY, store, &size);
            if (err != ESP_OK || store->magic != DIAG_MAGIC ||
                store->count > DIAG_RECORDS || store->next >= DIAG_RECORDS) {
                ESP_LOGE(TAG, "Saved diagnostic log is invalid; starting a new log");
                memset(store, 0, sizeof(*store));
                store->magic = DIAG_MAGIC;
            }
        } else if (err == ESP_OK) {
            ESP_LOGW(TAG, "Saved diagnostic log uses an older format; starting a new log");
        } else if (err != ESP_ERR_NVS_NOT_FOUND) {
            ESP_LOGE(TAG, "Could not read saved diagnostic log: %s", esp_err_to_name(err));
        }
        persistent = true;
    } else {
        ESP_LOGE(TAG, "Could not open diagnostic NVS namespace: %s", esp_err_to_name(err));
    }

    ESP_LOGI(TAG, "Previous BLE diagnostic log: %u records; oldest to newest",
             (unsigned)store->count);
    const uint16_t oldest = (uint16_t)((store->next + DIAG_RECORDS - store->count) % DIAG_RECORDS);
    for (uint16_t i = 0; i < store->count; ++i)
        print_record(&store->records[(oldest + i) % DIAG_RECORDS]);

    ++store->boot_id;
    disconnected_since_ms = now_ms();
    event_queue = xQueueCreateWithCaps(32, sizeof(pending_event_t),
                                       MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!event_queue) {
        ESP_LOGE(TAG, "Could not allocate diagnostic event queue; persistent logging disabled");
        persistent = false;
        return;
    }
    connection_diag_event(CONN_DIAG_BOOT, 0, 0);
}

void connection_diag_set_ble_state(bool connected, bool subscribed)
{
    const uint32_t now = now_ms();
    taskENTER_CRITICAL(&stats_lock);
    const bool was_connected = (link_flags & 1) != 0;
    link_flags = (connected ? 1 : 0) | (subscribed ? 2 : 0);
    if (connected && !was_connected) {
        connected_since_ms = now;
        session_max_callback_us = 0;
        session_max_queue_wait_ms = 0;
        session_max_parse_us = 0;
        session_queue_peak = 0;
        current_speed_kmh = -1;
        speed_peak_kmh = 0;
        last_status_ms = 0;
    } else if (!connected) {
        connected_since_ms = 0;
    }
    taskEXIT_CRITICAL(&stats_lock);
    if (connected) disconnected_since_ms = 0;
    else if (!disconnected_since_ms) disconnected_since_ms = now;
}

void connection_diag_note_rx(uint32_t bytes, uint8_t queue_depth,
                             uint32_t callback_time_us, bool accepted)
{
    taskENTER_CRITICAL(&stats_lock);
    if (accepted) {
        ++rx_writes;
        rx_bytes += bytes;
    } else {
        ++rx_rejected;
    }
    if (bytes > rx_max_bytes && bytes <= UINT16_MAX) rx_max_bytes = (uint16_t)bytes;
    if (callback_time_us > session_max_callback_us)
        session_max_callback_us = callback_time_us;
    if (queue_depth > session_queue_peak)
        session_queue_peak = queue_depth;
    taskEXIT_CRITICAL(&stats_lock);
}

void connection_diag_note_rx_processed(uint32_t queue_wait_ms, uint32_t parse_time_us)
{
    taskENTER_CRITICAL(&stats_lock);
    if (queue_wait_ms > session_max_queue_wait_ms)
        session_max_queue_wait_ms = queue_wait_ms;
    if (parse_time_us > session_max_parse_us)
        session_max_parse_us = parse_time_us;
    taskEXIT_CRITICAL(&stats_lock);
}

void connection_diag_note_status(int speed_kmh, bool has_speed)
{
    const uint32_t now = now_ms();
    taskENTER_CRITICAL(&stats_lock);
    ++status_frames;
    current_speed_kmh = has_speed && speed_kmh >= 0 ? (int16_t)speed_kmh : -1;
    if (current_speed_kmh >= 0 && (uint16_t)current_speed_kmh > speed_peak_kmh)
        speed_peak_kmh = (uint16_t)current_speed_kmh;
    last_status_ms = now;
    taskEXIT_CRITICAL(&stats_lock);
}

static void enqueue_event(const pending_event_t *event)
{
    if (!store) return;
    if (!event_queue) return;
    if (xQueueSend(event_queue, event, 0) != pdTRUE) {
        ESP_LOGE(TAG, "Diagnostic event queue full; event %s was not saved",
                 event_name(event->event));
    }
}

static pending_event_t snapshot_event(connection_diag_event_t event,
                                      int32_t arg0, int32_t arg1,
                                      uint8_t queue_depth, uint32_t status_age_ms,
                                      int8_t rssi, uint16_t host_stack_hwm,
                                      uint16_t protocol_stack_hwm)
{
    pending_event_t item = {
        .event = event,
        .uptime_ms = (uint32_t)(esp_timer_get_time() / 1000),
        .arg0 = arg0,
        .arg1 = arg1,
        .queue_depth = queue_depth,
        .status_age_ms = status_age_ms,
        .rssi = rssi,
        .host_stack_hwm = host_stack_hwm,
        .protocol_stack_hwm = protocol_stack_hwm,
    };
    const uint32_t now = now_ms();
    taskENTER_CRITICAL(&stats_lock);
    item.link_age_s = connected_since_ms ? (now - connected_since_ms) / 1000 : 0;
    if (rssi != -127) {
        last_rssi = rssi;
        last_rssi_ms = now;
    } else if (rssi == -127 && last_rssi != -127) {
        item.rssi = last_rssi;
    }
    item.rssi_age_s = last_rssi_ms ? (now - last_rssi_ms) / 1000 : UINT32_MAX;
    if (item.status_age_ms == UINT32_MAX)
        item.status_age_ms = last_status_ms ? now - last_status_ms : UINT32_MAX;
    item.speed_kmh = current_speed_kmh < 0 ? UINT16_MAX : (uint16_t)current_speed_kmh;
    item.speed_peak_kmh = speed_peak_kmh;
    if (event == CONN_DIAG_HEALTH)
        speed_peak_kmh = current_speed_kmh < 0 ? 0 : (uint16_t)current_speed_kmh;
    item.max_gatt_cb_us = session_max_callback_us;
    item.max_queue_wait_ms = session_max_queue_wait_ms;
    item.max_parse_us = session_max_parse_us;
    item.queue_peak = session_queue_peak;
    item.flags = link_flags;
    item.rx_writes = rx_writes;
    item.rx_bytes = rx_bytes;
    item.rx_rejected = rx_rejected;
    item.rx_max_bytes = rx_max_bytes;
    item.status_frames = status_frames;
    taskEXIT_CRITICAL(&stats_lock);
    item.free_internal = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    item.min_internal = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    item.largest_internal = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    item.free_psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    item.largest_psram = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return item;
}

void connection_diag_event(connection_diag_event_t event, int32_t arg0, int32_t arg1)
{
    pending_event_t item = snapshot_event(event, arg0, arg1, 0, UINT32_MAX, -127, 0, 0);
    enqueue_event(&item);
}

void connection_diag_link_end(int reason, int8_t rssi, uint8_t queue_depth)
{
    if (!store || !event_queue) return;
    pending_event_t item = snapshot_event(CONN_DIAG_DISCONNECT, reason, 0,
                                          queue_depth, UINT32_MAX, rssi, 0, 0);
    if (!event_queue || xQueueSendToFront(event_queue, &item, 0) != pdTRUE) {
        ESP_LOGE(TAG, "Diagnostic event queue full; disconnect record was not saved");
    }
}

void connection_diag_health(uint8_t queue_depth, uint32_t status_age_ms, int8_t rssi,
                            uint16_t host_stack_hwm, uint16_t protocol_stack_hwm)
{
    pending_event_t item = snapshot_event(CONN_DIAG_HEALTH, 0, 0, queue_depth,
                                          status_age_ms, rssi,
                                          host_stack_hwm, protocol_stack_hwm);
    enqueue_event(&item);
}

void connection_diag_process(void)
{
    pending_event_t item;
    if (event_queue && xQueueReceive(event_queue, &item, 0) == pdTRUE) {
        record_t record = {
            .boot_id = store->boot_id,
            .uptime_ms = item.uptime_ms,
            .uptime_s = item.uptime_ms / 1000,
            .status_age_s = item.status_age_ms == UINT32_MAX
                ? UINT32_MAX : item.status_age_ms / 1000,
            .free_internal = item.free_internal,
            .min_internal = item.min_internal,
            .largest_internal = item.largest_internal,
            .free_psram = item.free_psram,
            .largest_psram = item.largest_psram,
            .event = (uint16_t)item.event,
            .speed_kmh = item.speed_kmh,
            .speed_peak_kmh = item.speed_peak_kmh,
            .host_stack_hwm = item.host_stack_hwm,
            .protocol_stack_hwm = item.protocol_stack_hwm,
            .queue_depth = item.queue_depth,
            .queue_peak = item.queue_peak,
            .rssi = item.rssi,
            .flags = item.flags,
            .link_age_s = item.link_age_s,
            .rssi_age_s = item.rssi_age_s,
            .max_gatt_cb_us = item.max_gatt_cb_us,
            .max_queue_wait_ms = item.max_queue_wait_ms,
            .max_parse_us = item.max_parse_us,
            .arg0 = item.arg0,
            .arg1 = item.arg1,
        };
        record.rx_writes = item.rx_writes;
        record.rx_bytes = item.rx_bytes;
        record.rx_rejected = item.rx_rejected;
        record.rx_max_bytes = item.rx_max_bytes;
        record.status_frames = item.status_frames;

        store->records[store->next] = record;
        store->next = (uint16_t)((store->next + 1) % DIAG_RECORDS);
        if (store->count < DIAG_RECORDS) ++store->count;
        dirty = true;
        print_record(&record);
    }

    uint8_t current_link_flags;
    taskENTER_CRITICAL(&stats_lock);
    current_link_flags = link_flags;
    taskEXIT_CRITICAL(&stats_lock);
    const uint32_t now = now_ms();
    if (dirty && persistent && !(current_link_flags & 1) &&
        uxQueueMessagesWaiting(event_queue) == 0 &&
        disconnected_since_ms && now - disconnected_since_ms >= DIAG_IDLE_FLUSH_MS) {
        persist_store();
    }
}
