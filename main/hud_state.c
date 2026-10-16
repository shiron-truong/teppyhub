#include "hud_state.h"

#include "sdkconfig.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FRESH_MS        4000     /* BLE data older than this -> Idle layout, speed shows "--" */
#define ALERT_TTL_MS    12000    /* alert cards auto-hide 10-15 s after the last update */
#define HIST_STEP_S     150
#define DRIVING_MIN_KMH 3

static const char *TAG = "hud_state";
static SemaphoreHandle_t mtx;
static hud_state_t cur;
static hud_settings_t cfg;

typedef struct { hud_alert_t a; uint32_t seen_ms; } alert_slot_t;
static alert_slot_t slots[HUD_MAX_ALERTS];

static hud_stats_t stats;
static uint64_t drive_sum;
static uint32_t drive_cnt, win_sum, win_cnt, win_ticks;

static uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

/* ------------------------------------------------------------------ settings (NVS) */
static void settings_defaults(void)
{
    cfg = (hud_settings_t){ .brightness = 100, .sound = true, .auto_dim = true, .theme = 0, .lang = 0 };
}

static void settings_load(void)
{
    settings_defaults();
    nvs_handle_t h;
    if (nvs_open("hud", NVS_READONLY, &h) != ESP_OK) return;
    uint8_t v;
    if (nvs_get_u8(h, "br", &v) == ESP_OK) cfg.brightness = v;
    if (nvs_get_u8(h, "snd", &v) == ESP_OK) cfg.sound = v != 0;
    if (nvs_get_u8(h, "adim", &v) == ESP_OK) cfg.auto_dim = v != 0;
    if (nvs_get_u8(h, "thm", &v) == ESP_OK) cfg.theme = v ? 1 : 0;
    if (nvs_get_u8(h, "lng", &v) == ESP_OK) cfg.lang = v ? 1 : 0;
    nvs_close(h);
    if (cfg.brightness < 10) cfg.brightness = 10;
    if (cfg.brightness > 100) cfg.brightness = 100;
}

static void settings_save(const hud_settings_t *c)
{
    nvs_handle_t h;
    if (nvs_open("hud", NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u8(h, "br", c->brightness);
    nvs_set_u8(h, "snd", c->sound);
    nvs_set_u8(h, "adim", c->auto_dim);
    nvs_set_u8(h, "thm", c->theme);
    nvs_set_u8(h, "lng", c->lang);
    nvs_commit(h);
    nvs_close(h);
}

void hud_settings_set(const hud_settings_t *c, bool persist)
{
    hud_settings_t n = *c;
    if (n.brightness < 10) n.brightness = 10;
    if (n.brightness > 100) n.brightness = 100;
    xSemaphoreTake(mtx, portMAX_DELAY);
    const bool changed = memcmp(&n, &cfg, sizeof(n)) != 0;
    cfg = n;
    xSemaphoreGive(mtx);
    if (persist) settings_save(&n);
    (void)changed;
}

/* ------------------------------------------------------------------ alerts */
/* HLP/1 alert codes -> presentation families; preserve the raw code for its label/icon. */
static hud_alert_type_t alert_type_from_code(int code, uint8_t *level_default)
{
    *level_default = 0;
    switch (code) {
    case 1:  return HUD_ALERT_POLICE;
    case 2: case 3:
    case 40: case 41: case 42: case 43: case 44: case 45: case 46:
        return HUD_ALERT_CAMERA;
    case 5:  return HUD_ALERT_ACCIDENT;
    case 6:  return HUD_ALERT_JAM;
    case 7:  return HUD_ALERT_ROAD_CLOSED;
    default: return code > 0 ? HUD_ALERT_HAZARD : HUD_ALERT_NONE;
    }
}

static int alert_priority(const hud_alert_t *a)
{
    /* ROAD_CLOSED ~ ACCIDENT > POLICE > JAM heavy/standstill > HAZARD > JAM light/moderate */
    switch (a->type) {
    case HUD_ALERT_ROAD_CLOSED: return 100;
    case HUD_ALERT_ACCIDENT:    return 99;
    case HUD_ALERT_POLICE:      return 80;
    case HUD_ALERT_CAMERA:      return 75;
    case HUD_ALERT_JAM:         return a->level >= 4 ? 100 : a->level >= 2 ? 60 : 20;
    case HUD_ALERT_HAZARD:      return 40;
    default:                    return 0;
    }
}

static void alerts_touch(int code, int subtype, int distance_m, int value, int delay_min, uint32_t t)
{
    uint8_t lvl;
    hud_alert_type_t type = alert_type_from_code(code, &lvl);
    if (type == HUD_ALERT_NONE) return;
    if (type == HUD_ALERT_JAM && subtype >= 1 && subtype <= 5)
        lvl = (uint8_t)(subtype - 1);
    else if (subtype >= 0 && subtype <= 3)
        lvl = (uint8_t)subtype;
    hud_alert_t a = { .type = type, .code = code, .level = lvl,
                      .distance_m = distance_m, .value = value,
                      .delay_min = delay_min };

    alert_slot_t *victim = NULL;
    for (int i = 0; i < HUD_MAX_ALERTS; ++i) {
        if (slots[i].a.type == type && slots[i].a.code == a.code) { victim = &slots[i]; break; }
    }
    if (!victim) {
        for (int i = 0; i < HUD_MAX_ALERTS; ++i) if (slots[i].a.type == HUD_ALERT_NONE) { victim = &slots[i]; break; }
    }
    if (!victim) {   /* full: replace the lowest priority, oldest card */
        victim = &slots[0];
        for (int i = 1; i < HUD_MAX_ALERTS; ++i) {
            const int pv = alert_priority(&victim->a), pi = alert_priority(&slots[i].a);
            if (pi < pv || (pi == pv && slots[i].seen_ms < victim->seen_ms)) victim = &slots[i];
        }
    }
    victim->a = a;
    victim->seen_ms = t;
}

int hud_alerts_get(hud_alert_t out[HUD_MAX_ALERTS])
{
    const uint32_t t = now_ms();
    int n = 0;
    xSemaphoreTake(mtx, portMAX_DELAY);
    for (int i = 0; i < HUD_MAX_ALERTS; ++i) {
        if (slots[i].a.type == HUD_ALERT_NONE) continue;
        if (t - slots[i].seen_ms > ALERT_TTL_MS) { memset(&slots[i], 0, sizeof(slots[i])); continue; }
        out[n++] = slots[i].a;
    }
    xSemaphoreGive(mtx);
    for (int i = 1; i < n; ++i) {            /* tiny insertion sort: priority desc, distance asc */
        hud_alert_t k = out[i];
        int j = i - 1;
        while (j >= 0) {
            const int pj = alert_priority(&out[j]), pk = alert_priority(&k);
            const bool before = pk > pj || (pk == pj && k.distance_m >= 0 && (out[j].distance_m < 0 || k.distance_m < out[j].distance_m));
            if (!before) break;
            out[j + 1] = out[j];
            --j;
        }
        out[j + 1] = k;
    }
    return n;
}

/* ------------------------------------------------------------------ state in / snapshot out */
void hud_state_set_ble_connected(bool connected)
{
    xSemaphoreTake(mtx, portMAX_DELAY);
    cur.ble_connected = connected;
    if (!connected) memset(slots, 0, sizeof(slots));
    xSemaphoreGive(mtx);
}

void hud_state_update(const hud_state_t *state)
{
    const uint32_t t = now_ms();
    xSemaphoreTake(mtx, portMAX_DELAY);
    const bool connected = cur.ble_connected;
    cur = *state;
    cur.ble_connected = connected;
    cur.updated_ms = t;
    /* HLP status messages are snapshots: replace prior alerts, including on an empty list. */
    memset(slots, 0, sizeof(slots));
    if (state->has_alert_list) {
        for (int i = 0; i < state->alert_count && i < HUD_MAX_ALERTS; ++i) {
            alerts_touch(state->alert_codes[i], state->alert_subtypes[i],
                         state->alert_distances[i], state->alert_values[i],
                         state->alert_delays[i], t);
        }
    } else if (state->alert_code > 0) {
        alerts_touch(state->alert_code, state->alert_subtype, state->alert_distance_m,
                     state->alert_value, state->alert_delay_min, t);
    }
    xSemaphoreGive(mtx);
}

static bool clock_is_night(const char *txt, bool *known)
{
    int h = -1, m = -1;
    *known = false;
    if (sscanf(txt, "%d:%d", &h, &m) != 2 || h < 0 || h > 23) return false;
    *known = true;
    return h >= 18 || h < 6;                 /* day 06:00-17:59 */
}

void hud_state_get(hud_snapshot_t *out)
{
    xSemaphoreTake(mtx, portMAX_DELAY);
    out->s = cur;
    out->cfg = cfg;
    xSemaphoreGive(mtx);

    const hud_state_t *s = &out->s;
    out->fresh = s->ble_connected && s->updated_ms && (now_ms() - s->updated_ms < FRESH_MS);
    out->nav_mode = out->fresh && s->navigation;
    const int margin = CONFIG_HUD_OVERSPEED_MARGIN_KMH;
    out->over_limit = out->fresh && (s->has_over ? s->waze_over :
        s->has_speed && s->has_limit && s->speed_kmh > s->limit_kmh + margin);
    out->over_by_kmh = out->over_limit ? s->speed_kmh - s->limit_kmh : 0;
    bool known;
    out->night = clock_is_night(s->clock_text, &known);
    /* Night: cap at 20 % (spec: 15-25 %). Backlight is on/off only on this board, so the UI dims in software. */
    int b = out->cfg.brightness;
    if (out->cfg.auto_dim && out->night && b > 20) b = 20;
    out->brightness_pct = b;
}

/* ------------------------------------------------------------------ statistics */
static void stats_tick(void *arg)
{
    (void)arg;
    hud_snapshot_t snap;
    hud_state_get(&snap);
    xSemaphoreTake(mtx, portMAX_DELAY);
    if (snap.fresh && snap.s.has_speed) {
        const int v = snap.s.speed_kmh;
        if (v >= DRIVING_MIN_KMH) {
            stats.drive_s++;
            drive_sum += (uint64_t)v;
            drive_cnt++;
            stats.avg_kmh = (int)((drive_sum + drive_cnt / 2) / drive_cnt);
        }
        if (v > stats.max_kmh) stats.max_kmh = v;
        win_sum += (uint32_t)v;
        win_cnt++;
        if (++win_ticks >= HIST_STEP_S) {
            const int avg = win_cnt ? (int)(win_sum / win_cnt) : 0;
            if (stats.hist_n < HUD_HIST_N) stats.hist[stats.hist_n++] = avg;
            else { memmove(stats.hist, stats.hist + 1, sizeof(int) * (HUD_HIST_N - 1)); stats.hist[HUD_HIST_N - 1] = avg; }
            win_sum = win_cnt = win_ticks = 0;
        }
    }
    xSemaphoreGive(mtx);
}

void hud_stats_get(hud_stats_t *out)
{
    xSemaphoreTake(mtx, portMAX_DELAY);
    *out = stats;
    xSemaphoreGive(mtx);
}

void hud_stats_seed_demo(void)
{
    static const int demo[HUD_HIST_N] = { 48, 62, 55, 70, 82, 68, 55, 76, 88, 74, 62, 58, 52 };
    xSemaphoreTake(mtx, portMAX_DELAY);
    memcpy(stats.hist, demo, sizeof(demo));
    stats.hist_n = HUD_HIST_N;
    stats.avg_kmh = 62; stats.max_kmh = 118; stats.drive_s = 1 * 3600 + 42 * 60;
    drive_sum = 62ull * 6000; drive_cnt = 6000;
    xSemaphoreGive(mtx);
}

void hud_state_init(void)
{
    memset(&cur, 0, sizeof(cur));
    cur.distance_m = cur.remaining_m = cur.progress_percent = -1;
    cur.alert_subtype = -1;
    cur.lane_recommended = -1;
    memset(slots, 0, sizeof(slots));
    memset(&stats, 0, sizeof(stats));
    mtx = xSemaphoreCreateMutex();
    configASSERT(mtx);
    settings_load();

    static esp_timer_handle_t timer;
    const esp_timer_create_args_t args = { .callback = stats_tick, .name = "hud_stats" };
    if (esp_timer_create(&args, &timer) == ESP_OK) esp_timer_start_periodic(timer, 1000000);
    ESP_LOGI(TAG, "state ready (brightness=%u sound=%d theme=%u lang=%u)", cfg.brightness, cfg.sound, cfg.theme, cfg.lang);
}
