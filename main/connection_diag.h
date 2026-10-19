#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    CONN_DIAG_BOOT = 1,
    CONN_DIAG_BLE_START,
    CONN_DIAG_BLE_SYNC,
    CONN_DIAG_ADV_START,
    CONN_DIAG_ADV_FAIL,
    CONN_DIAG_CONNECT,
    CONN_DIAG_CONNECT_FAIL,
    CONN_DIAG_DISCONNECT,
    CONN_DIAG_SUBSCRIBE,
    CONN_DIAG_UNSUBSCRIBE,
    CONN_DIAG_RX_QUEUE_FULL,
    CONN_DIAG_BAD_JSON,
    CONN_DIAG_UNKNOWN_FRAME,
    CONN_DIAG_FRAME_OVERFLOW,
    CONN_DIAG_DATA_STALE,
    CONN_DIAG_DATA_RESUMED,
    CONN_DIAG_HEALTH,
    CONN_DIAG_HOST_STOPPED,
    CONN_DIAG_GATT_ERROR,
    CONN_DIAG_CONN_UPDATE,
    CONN_DIAG_ADV_COMPLETE,
    CONN_DIAG_NO_NOTIFY,
} connection_diag_event_t;

void connection_diag_init(void);
void connection_diag_set_ble_state(bool connected, bool subscribed);
void connection_diag_note_rx(uint32_t bytes, uint8_t queue_depth,
                             uint32_t callback_time_us, bool accepted);
void connection_diag_note_rx_processed(uint32_t queue_wait_ms, uint32_t parse_time_us);
void connection_diag_note_status(int speed_kmh, bool has_speed);
void connection_diag_event(connection_diag_event_t event, int32_t arg0, int32_t arg1);
void connection_diag_link_end(int reason, int8_t rssi, uint8_t queue_depth);
void connection_diag_health(uint8_t queue_depth, uint32_t status_age_ms, int8_t rssi,
                            uint16_t host_stack_hwm, uint16_t protocol_stack_hwm);
void connection_diag_process(void);
