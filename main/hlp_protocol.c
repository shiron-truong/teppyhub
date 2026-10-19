#include "hlp_protocol.h"
#include "hud_state.h"
#include "connection_diag.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "cJSON.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define HLP_MAX_FRAME 512
static const char *TAG = "hlp";
static char frame[HLP_MAX_FRAME];
static size_t used;
static uint32_t last_status_log_ms;
static bool logged_status_fields;
static volatile bool session_reset_requested;
static uint32_t last_status_ms;
static uint32_t last_bad_json_log_ms;
static uint32_t last_unknown_log_ms;
static uint32_t bad_json_count;
static bool logged_bad_json;
static bool logged_unknown;

static cJSON *field(cJSON *root, const char *a, const char *b, const char *c) {
    cJSON *v = cJSON_GetObjectItemCaseSensitive(root, a);
    if (!v && b) v = cJSON_GetObjectItemCaseSensitive(root, b);
    if (!v && c) v = cJSON_GetObjectItemCaseSensitive(root, c);
    return v;
}
static int integer(cJSON *root, const char *a, const char *b, const char *c, int fallback) {
    cJSON *v = field(root, a, b, c);
    if (cJSON_IsNumber(v)) return v->valueint;
    if (cJSON_IsString(v) && v->valuestring) return atoi(v->valuestring);
    return fallback;
}
static void string_value(cJSON *root, char *out, size_t size, const char *a, const char *b, const char *c) {
    cJSON *v = field(root, a, b, c);
    if (!cJSON_IsString(v) || !v->valuestring) { out[0] = 0; return; }
    snprintf(out, size, "%s", v->valuestring);
}

static bool has_value(cJSON *root, const char *a, const char *b, const char *c) {
    cJSON *v = field(root, a, b, c);
    return cJSON_IsNumber(v) || cJSON_IsString(v);
}

static bool parse_clock_minutes(const char *text, int *minutes) {
    int hour, minute;
    char tail;
    if (sscanf(text, "%d:%d%c", &hour, &minute, &tail) != 2 ||
        hour < 0 || hour > 23 || minute < 0 || minute > 59) return false;
    *minutes = hour * 60 + minute;
    return true;
}

static int remaining_distance(cJSON *root) {
    cJSON *m = field(root, "rm", "remainingM", "remaining_m");
    if (cJSON_IsNumber(m)) return m->valueint;
    if (cJSON_IsString(m) && m->valuestring) return atoi(m->valuestring);
    cJSON *km = field(root, "rkm", "remainingKm", "remaining_km");
    if (cJSON_IsNumber(km)) return (int)(km->valuedouble * 1000.0 + 0.5);
    if (cJSON_IsString(km) && km->valuestring) return (int)(atof(km->valuestring) * 1000.0 + 0.5);
    return -1;
}

static int lane_arrow_from_mask(int mask) {
    if (mask & 128) return HUD_LANE_UTURN;
    if (mask & (4 | 8)) return HUD_LANE_LEFT;
    if (mask & 2) return HUD_LANE_SLIGHT_LEFT;
    if (mask & (32 | 64)) return HUD_LANE_RIGHT;
    if (mask & 16) return HUD_LANE_SLIGHT_RIGHT;
    if (mask & 1) return HUD_LANE_STRAIGHT;
    return -1;
}

static void parse_lanes(cJSON *root, hud_state_t *s) {
    cJSON *container = field(root, "lan", "lanes", "laneGuidance");
    if (!cJSON_IsArray(container)) return;

    const int count = cJSON_GetArraySize(container);
    for (int i = 0; i < count && s->lane_count < HUD_MAX_LANES; ++i) {
        cJSON *item = cJSON_GetArrayItem(container, i);
        if (!cJSON_IsArray(item) || cJSON_GetArraySize(item) < 2) continue;
        cJSON *directions = cJSON_GetArrayItem(item, 0);
        cJSON *selected = cJSON_GetArrayItem(item, 1);
        if (!cJSON_IsNumber(directions) || !cJSON_IsNumber(selected)) continue;
        const int selected_mask = selected->valueint & directions->valueint;
        const int value = lane_arrow_from_mask(selected_mask ? selected_mask : directions->valueint);
        if (value < 0) continue;
        const int index = s->lane_count++;
        s->lanes[index] = (uint8_t)value;
        if (selected_mask) s->lane_recommended = (int8_t)index;
    }
}

static void parse_alert_list(cJSON *root, hud_state_t *s) {
    cJSON *items = cJSON_GetObjectItemCaseSensitive(root, "alrs");
    if (!cJSON_IsArray(items)) return;
    s->has_alert_list = true;
    const int count = cJSON_GetArraySize(items);
    for (int i = 0; i < count && s->alert_count < HUD_MAX_ALERTS; ++i) {
        cJSON *item = cJSON_GetArrayItem(items, i);
        if (!cJSON_IsObject(item)) continue;
        const int code = integer(item, "k", NULL, NULL, 0);
        if (code <= 0) continue;
        const int index = s->alert_count++;
        s->alert_codes[index] = code;
        s->alert_distances[index] = integer(item, "d", NULL, NULL, -1);
        s->alert_values[index] = integer(item, "v", NULL, NULL, 0);
        s->alert_subtypes[index] = integer(item, "s", NULL, NULL, -1);
        s->alert_delays[index] = integer(item, "m", NULL, NULL, -1);
    }
}

static void handle_frame(const char *line, size_t length) {
    cJSON *root = cJSON_ParseWithLength(line, length);
    if (!root) {
        ++bad_json_count;
        const uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
        if (!logged_bad_json || now - last_bad_json_log_ms >= 5000) {
            last_bad_json_log_ms = now;
            logged_bad_json = true;
            ESP_LOGW(TAG, "Ignoring malformed HLP JSON frame (length=%u, count=%u)",
                     (unsigned)length, (unsigned)bad_json_count);
            connection_diag_event(CONN_DIAG_BAD_JSON, (int32_t)length,
                                  (int32_t)bad_json_count);
        }
        return;
    }
    cJSON *type = cJSON_GetObjectItemCaseSensitive(root, "t");
    if (cJSON_IsString(type) && strcmp(type->valuestring, "ping") == 0) {
        hlp_protocol_send_line("{\"v\":1,\"t\":\"pong\"}");
        cJSON_Delete(root);
        return;
    }
    if (cJSON_IsString(type) && strcmp(type->valuestring, "s") == 0) {
        last_status_ms = (uint32_t)(esp_timer_get_time() / 1000);
        hud_state_t s = { .distance_m = -1, .remaining_m = -1, .remaining_min = -1, .progress_percent = -1, .avg_zone_remaining_m = -1, .avg_zone_progress = -1, .alert_subtype = -1, .alert_delay_min = -1, .lane_recommended = -1, .updated_ms = (uint32_t)(esp_timer_get_time() / 1000) };
        if (!logged_status_fields) {
            char names[256] = {0};
            size_t offset = 0;
            for (const cJSON *item = root->child; item; item = item->next) {
                if (!item->string) continue;
                int written = snprintf(names + offset, sizeof(names) - offset, "%s%s",
                                       offset ? "," : "", item->string);
                if (written < 0 || (size_t)written >= sizeof(names) - offset) break;
                offset += (size_t)written;
            }
            ESP_LOGI(TAG, "Waze status fields: %s", names);
            logged_status_fields = true;
        }
        s.speed_kmh = integer(root, "spd", "speed", "speedKmh", 0);
        s.limit_kmh = integer(root, "lim", "limit", "speedLimit", 0);
        s.waze_over = integer(root, "over", NULL, NULL, 0) != 0;
        s.has_over = has_value(root, "over", NULL, NULL);
        s.has_speed = has_value(root, "spd", "speed", "speedKmh") && s.speed_kmh >= 0;
        connection_diag_note_status(s.speed_kmh, s.has_speed);
        s.has_limit = has_value(root, "lim", "limit", "speedLimit") && s.limit_kmh > 0;
        s.distance_m = integer(root, "dst", "dist", "distanceM", -1);
        s.remaining_m = remaining_distance(root);
        s.has_remaining = s.remaining_m >= 0;
        s.progress_percent = integer(root, "progress", "pct", "routePct", -1);
        s.route_total_m = integer(root, "totalM", "routeM", "tdst", -1);
        if (s.progress_percent < 0 || s.progress_percent > 100) {
            s.progress_percent = -1;
            if (s.route_total_m > 0 && s.has_remaining &&
                s.remaining_m <= s.route_total_m) {
                const int64_t traveled_m = (int64_t)s.route_total_m - s.remaining_m;
                s.progress_percent = (int)(traveled_m * 100 / s.route_total_m);
            }
        }
        s.turn_code = integer(root, "trn", "turn", "maneuver", 0);
        s.roundabout_exit = integer(root, "exit", NULL, NULL, 0);
        s.avg_zone = integer(root, "avg", NULL, NULL, 0) != 0;
        s.avg_zone_remaining_m = integer(root, "avgL", NULL, NULL, -1);
        s.avg_zone_speed_kmh = integer(root, "avgR", NULL, NULL, 0);
        s.avg_zone_progress = integer(root, "avgP", NULL, NULL, -1);
        if (s.avg_zone_progress < 0 || s.avg_zone_progress > 100) s.avg_zone_progress = -1;
        parse_lanes(root, &s);
        s.alert_code = integer(root, "alr", NULL, NULL, 0);
        s.alert_distance_m = integer(root, "alrD", NULL, NULL, -1);
        s.alert_value = integer(root, "alrV", NULL, NULL, 0);
        s.alert_subtype = integer(root, "alrS", NULL, NULL, -1);
        s.alert_delay_min = integer(root, "alrM", NULL, NULL, -1);
        parse_alert_list(root, &s);
        string_value(root, s.street, sizeof(s.street), "st", "street", NULL);
        string_value(root, s.next_street, sizeof(s.next_street), "st2", "nextStreet", NULL);
        string_value(root, s.eta, sizeof(s.eta), "eta", NULL, NULL);
        string_value(root, s.clock_text, sizeof(s.clock_text), "time", "clock", NULL);
        s.remaining_min = integer(root, "rmin", "remainingMin", "remaining_min", -1);
        if (!s.clock_text[0]) {
            int hour = integer(root, "hour", NULL, NULL, -1);
            int minute = integer(root, "minute", "min", NULL, -1);
            if (hour >= 0 && hour < 24 && minute >= 0 && minute < 60)
                snprintf(s.clock_text, sizeof(s.clock_text), "%02d:%02d", hour, minute);
        }
        if (!s.clock_text[0]) {
            int eta_minutes;
            if (s.remaining_min >= 0 && parse_clock_minutes(s.eta, &eta_minutes)) {
                const int clock_minutes = (eta_minutes - s.remaining_min + 24 * 60) % (24 * 60);
                snprintf(s.clock_text, sizeof(s.clock_text), "%02d:%02d",
                         clock_minutes / 60, clock_minutes % 60);
            }
        }
        if (s.remaining_min < 0) {
            int eta_minutes, clock_minutes;
            if (parse_clock_minutes(s.eta, &eta_minutes) &&
                parse_clock_minutes(s.clock_text, &clock_minutes)) {
                s.remaining_min = (eta_minutes - clock_minutes + 24 * 60) % (24 * 60);
            }
        }
        s.navigation = integer(root, "nav", NULL, NULL, 0) != 0 || s.turn_code != 0 ||
                       s.roundabout_exit > 0 || s.distance_m >= 0 || s.has_remaining ||
                       s.remaining_min >= 0 || s.progress_percent >= 0 || s.avg_zone ||
                       s.eta[0] || s.street[0] || s.next_street[0] || s.lane_count > 0;
        hud_state_update(&s);
        if ((uint32_t)(esp_timer_get_time() / 1000) - last_status_log_ms >= 2000) {
            last_status_log_ms = (uint32_t)(esp_timer_get_time() / 1000);
            char speed[12], limit[12];
            if (s.has_speed) snprintf(speed, sizeof(speed), "%d", s.speed_kmh); else snprintf(speed, sizeof(speed), "--");
            if (s.has_limit) snprintf(limit, sizeof(limit), "%d", s.limit_kmh); else snprintf(limit, sizeof(limit), "--");
            ESP_LOGI(TAG, "Waze data: speed=%s limit=%s nav=%d turn=%d turn_m=%d remaining_m=%d progress=%d route_total_m=%d time=%s eta=%s remaining_min=%d alert=%d sub=%d alrs=%u first=%d/%dm",
                speed, limit,
                s.navigation, s.turn_code, s.distance_m, s.remaining_m, s.progress_percent,
                s.route_total_m, s.clock_text[0] ? s.clock_text : "--:--",
                s.eta, s.remaining_min,
                s.alert_code, s.alert_subtype, (unsigned)s.alert_count,
                s.alert_count ? s.alert_codes[0] : 0,
                s.alert_count ? s.alert_distances[0] : -1);
        }
    } else if (cJSON_IsString(type)) {
        const uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
        if (!logged_unknown || now - last_unknown_log_ms >= 5000) {
            last_unknown_log_ms = now;
            logged_unknown = true;
            ESP_LOGW(TAG, "Ignoring unsupported HLP frame type: %s", type->valuestring);
            connection_diag_event(CONN_DIAG_UNKNOWN_FRAME, 0, 0);
        }
    } else {
        const uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
        if (!logged_unknown || now - last_unknown_log_ms >= 5000) {
            last_unknown_log_ms = now;
            logged_unknown = true;
            ESP_LOGW(TAG, "Ignoring HLP JSON frame without a string type");
            connection_diag_event(CONN_DIAG_UNKNOWN_FRAME, 1, 0);
        }
    }
    cJSON_Delete(root);
}

void hlp_protocol_init(void)
{
    used = 0;
    last_status_ms = 0;
    session_reset_requested = false;
}

void hlp_protocol_reset_session(void) { session_reset_requested = true; }

bool hlp_protocol_take_session_reset(void)
{
    if (!session_reset_requested) return false;
    session_reset_requested = false;
    used = 0;
    last_status_ms = 0;
    return true;
}

uint32_t hlp_protocol_status_age_ms(uint32_t now_ms)
{
    return last_status_ms ? now_ms - last_status_ms : UINT32_MAX;
}

void hlp_protocol_feed(const uint8_t *data, size_t length) {
    for (size_t i = 0; i < length; ++i) {
        uint8_t ch = data[i];
        if (ch == '\n') {
            if (used && frame[used - 1] == '\r') --used;
            if (used) handle_frame(frame, used);
            used = 0;
        } else if (used < sizeof(frame) - 1) frame[used++] = (char)ch;
        else {
            const int32_t frame_length = (int32_t)(used + 1);
            used = 0;
            ESP_LOGW(TAG, "HLP frame exceeded %u bytes; discarded", HLP_MAX_FRAME);
            connection_diag_event(CONN_DIAG_FRAME_OVERFLOW, frame_length, 0);
        }
    }
}
