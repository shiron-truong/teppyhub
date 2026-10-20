#pragma once
/*
 * HUD data model.
 *
 *  hlp_protocol (BLE task)  --hud_state_update()-->  [ model + mutex ]  <--hud_state_get()--  UI timer / audio task
 *
 * The UI is never pushed to from the BLE thread: it polls a consistent snapshot. That avoids taking
 * the LVGL mutex from NimBLE and means a busy LVGL task can never drop a BLE update.
 */
#include <stdbool.h>
#include <stdint.h>

#define HUD_MAX_ALERTS 3
#define HUD_MAX_LANES  6
#define HUD_HIST_N     13          /* 13 points x 150 s = last 30 minutes */
#define HUD_DEBUG_FIELDS_LEN 384

typedef struct {
    bool ble_connected;
    bool navigation;
    bool has_speed;
    bool has_limit;
    bool has_remaining;
    bool has_over;
    bool waze_over;
    int speed_kmh;
    int limit_kmh;
    int distance_m;
    int remaining_m;
    int progress_percent;
    int route_total_m;
    int turn_code;
    int roundabout_exit;
    bool avg_zone;
    int avg_zone_remaining_m;
    int avg_zone_speed_kmh;
    int avg_zone_progress;
    int alert_code;
    int alert_distance_m;
    int alert_value;
    int alert_delay_min;
    int alert_subtype;               /* "alrS" (-1 when absent) */
    bool has_alert_list;
    uint8_t alert_count;
    int alert_codes[HUD_MAX_ALERTS];
    int alert_distances[HUD_MAX_ALERTS];
    int alert_values[HUD_MAX_ALERTS];
    int alert_subtypes[HUD_MAX_ALERTS];
    int alert_delays[HUD_MAX_ALERTS];
    char street[96];
    char next_street[96];
    char eta[16];
    int remaining_min;
    char clock_text[8];
    /* Lane guidance. lanes[i] is a hud_lane_t; lane_recommended = index or -1. */
    uint8_t lane_count;
    uint8_t lanes[HUD_MAX_LANES];
    int8_t  lane_recommended;
    uint32_t updated_ms;
} hud_state_t;

typedef enum {
    HUD_LANE_STRAIGHT = 0, HUD_LANE_LEFT, HUD_LANE_RIGHT,
    HUD_LANE_SLIGHT_LEFT, HUD_LANE_SLIGHT_RIGHT, HUD_LANE_UTURN
} hud_lane_t;

/* ---- HLP alerts ---- */
typedef enum {
    HUD_ALERT_NONE = 0, HUD_ALERT_POLICE, HUD_ALERT_CAMERA, HUD_ALERT_JAM,
    HUD_ALERT_ACCIDENT, HUD_ALERT_HAZARD, HUD_ALERT_ROAD_CLOSED, HUD_ALERT_AVG_ZONE
} hud_alert_type_t;

/* POLICE level: 0 visible, 1 hiding, 2 camera. JAM level: 0 light .. 4 road closed. */
typedef struct {
    hud_alert_type_t type;
    int code;                        /* raw HLP "alr" code, used for the label of HAZARD-like cards */
    uint8_t level;
    int distance_m;                  /* -1 unknown */
    int value;
    int delay_min;                   /* HLP alrM; -1 if not supplied */
} hud_alert_t;

/* ---- statistics ---- */
typedef struct {
    int avg_kmh, max_kmh;
    uint32_t drive_s;
    int hist[HUD_HIST_N];
    int hist_n;
} hud_stats_t;

/* ---- persisted settings ---- */
typedef struct {
    uint8_t brightness;              /* 10..100 */
    bool sound;
    bool auto_dim;
    uint8_t theme;                   /* 0 dark, 1 sport */
    uint8_t lang;                    /* 0 vi, 1 en */
    bool debug_mode;
} hud_settings_t;

/* ---- what the UI / audio consume ---- */
typedef struct {
    hud_state_t s;
    bool fresh;                      /* BLE up and data < 4 s old */
    bool has_ble_rssi;
    int ble_signal_pct;              /* RSSI mapped to a clamped 0-100 reference scale */
    bool nav_mode;                   /* fresh && navigation -> Waze Active layout, else Idle layout */
    bool over_limit;
    int over_by_kmh;
    bool night;                      /* phone clock says 18:00-05:59 */
    int brightness_pct;              /* effective (night / auto-dim applied) */
    hud_settings_t cfg;
} hud_snapshot_t;

void hud_state_init(void);
void hud_state_set_ble_connected(bool connected);
void hud_state_set_ble_rssi(int8_t rssi);
void hud_state_update(const hud_state_t *state);
void hud_state_get(hud_snapshot_t *out);
int  hud_alerts_get(hud_alert_t out[HUD_MAX_ALERTS]);    /* sorted by priority, returns count */
void hud_stats_get(hud_stats_t *out);
void hud_stats_seed_demo(void);
void hud_settings_set(const hud_settings_t *cfg, bool persist);  /* clamps; persist=true also writes NVS */

/* demo generator (started from main only when CONFIG_HUD_DEMO_MODE=y) */
void hud_demo_start(void);

/* audio (MAX98357A) */
void audio_start(void);
void audio_request_test(void);
