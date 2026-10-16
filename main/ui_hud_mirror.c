#include "ui_common.h"
#include "alert_icons.h"

#include <stdio.h>
#include <string.h>

typedef struct {
    lv_obj_t *root;
    lv_obj_t *status;
    lv_obj_t *speed, *kmh;
    lv_obj_t *limit, *limit_value;
    lv_obj_t *turn, *turn_distance, *roundabout_exit;
    lv_obj_t *alert, *alert_distance;
    lv_obj_t *route, *progress;
} mirror_hud_t;

static mirror_hud_t m;

static void fmt_dist(char *out, size_t n, int meters)
{
    if (meters < 0) out[0] = '\0';
    else if (meters >= 1000) snprintf(out, n, "%d.%d km", meters / 1000, (meters % 1000) / 100);
    else snprintf(out, n, "%d m", meters);
}

static void fmt_duration(char *out, size_t n, int minutes)
{
    if (minutes < 0) out[0] = '\0';
    else if (minutes < 60) snprintf(out, n, "(%dp)", minutes);
    else snprintf(out, n, "(%dh%02dp)", minutes / 60, minutes % 60);
}

lv_obj_t *ui_hud_mirror_create(lv_obj_t *parent)
{
    memset(&m, 0, sizeof(m));
    const ui_palette_t *p = ui_pal;
    lv_obj_t *root = m.root = lv_obj_create(parent);
    ui_page_clean(root);
    lv_obj_set_size(root, 800, 480);
    lv_obj_set_style_bg_color(root, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);

    m.status = ui_label(root, "", ui_f_small, p->dim);
    lv_obj_set_width(m.status, 420);
    lv_obj_set_style_text_align(m.status, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(m.status, 190, 20);

    m.speed = ui_label(root, "--", ui_f_idle, 0xFFFFFF);
    lv_obj_set_width(m.speed, 740);
    lv_obj_set_style_text_align(m.speed, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(m.speed, 30, 40);
    m.kmh = ui_label(root, ui_tr(S_KMH), ui_f_title, 0xFFFFFF);
    lv_obj_set_width(m.kmh, 300);
    lv_obj_set_style_text_align(m.kmh, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(m.kmh, 250, 274);

    m.limit = ui_box(root, 44, 120, 132, 132, 0x000000, LV_RADIUS_CIRCLE);
    lv_obj_set_style_border_width(m.limit, 9, 0);
    lv_obj_set_style_border_color(m.limit, lv_color_hex(0xE02B2B), 0);
    m.limit_value = ui_label(m.limit, "--", ui_f_big, 0xFFFFFF);
    lv_obj_center(m.limit_value);
    lv_obj_add_flag(m.limit, LV_OBJ_FLAG_HIDDEN);

    m.turn = ui_arrow_create(root, 116);
    lv_obj_set_pos(m.turn, 614, 94);
    lv_obj_add_flag(m.turn, LV_OBJ_FLAG_HIDDEN);
    m.turn_distance = ui_label(root, "", ui_f_big, 0xFFFFFF);
    lv_obj_set_width(m.turn_distance, 250);
    lv_obj_set_style_text_align(m.turn_distance, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(m.turn_distance, 555, 204);
    lv_obj_add_flag(m.turn_distance, LV_OBJ_FLAG_HIDDEN);
    m.roundabout_exit = ui_label(root, "", ui_f_big, 0xFFFFFF);
    lv_obj_set_width(m.roundabout_exit, 250);
    lv_obj_set_style_text_align(m.roundabout_exit, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(m.roundabout_exit, 555, 258);
    lv_obj_add_flag(m.roundabout_exit, LV_OBJ_FLAG_HIDDEN);

    m.alert = lv_img_create(root);
    lv_obj_set_size(m.alert, 76, 76);
    lv_obj_set_pos(m.alert, 94, 284);
    lv_obj_add_flag(m.alert, LV_OBJ_FLAG_HIDDEN);
    m.alert_distance = ui_label(root, "", ui_f_title, 0xFFFFFF);
    lv_obj_set_width(m.alert_distance, 190);
    lv_obj_set_style_text_align(m.alert_distance, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(m.alert_distance, 38, 360);
    lv_obj_add_flag(m.alert_distance, LV_OBJ_FLAG_HIDDEN);

    m.route = ui_label(root, "", ui_f_big, 0xFFFFFF);
    lv_obj_set_width(m.route, 760);
    lv_obj_set_style_text_align(m.route, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(m.route, 20, 354);
    m.progress = lv_bar_create(root);
    lv_obj_set_size(m.progress, 720, 12);
    lv_obj_set_pos(m.progress, 40, 442);
    lv_bar_set_range(m.progress, 0, 100);
    lv_obj_set_style_bg_color(m.progress, lv_color_hex(0x505050), LV_PART_MAIN);
    lv_obj_set_style_bg_color(m.progress, lv_color_white(), LV_PART_INDICATOR);
    lv_obj_add_flag(m.progress, LV_OBJ_FLAG_HIDDEN);
    return root;
}

void ui_hud_mirror_update(const hud_snapshot_t *sn)
{
    const hud_state_t *s = &sn->s;
    char text[80], distance[20], duration[20];

    const char *status = !s->ble_connected ? ui_tr(S_BLE_OFF) :
                         !sn->fresh ? ui_tr(S_BLE_WAIT) : ui_tr(S_LIVE);
    ui_set_text(m.status, status);

    if (sn->fresh && s->has_speed) snprintf(text, sizeof(text), "%d", s->speed_kmh);
    else snprintf(text, sizeof(text), "--");
    ui_set_text(m.speed, text);
    lv_obj_set_style_text_color(m.speed, lv_color_hex(sn->over_limit ? 0xFF3B30 : ui_pal->text), 0);

    if (sn->fresh && s->has_limit) {
        snprintf(text, sizeof(text), "%d", s->limit_kmh);
        ui_set_text(m.limit_value, text);
        lv_obj_clear_flag(m.limit, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(m.limit, LV_OBJ_FLAG_HIDDEN);
    }

    const bool nav = sn->fresh && sn->nav_mode;
    if (nav) {
        const bool roundabout = s->roundabout_exit > 0 ||
            (s->turn_code >= 10 && s->turn_code <= 12) ||
            s->turn_code == 19 || s->turn_code == 20;
        const bool has_turn = s->turn_code > 0 && s->turn_code <= 20 && s->turn_code != 18;
        if (has_turn || roundabout) {
            ui_arrow_t direction = has_turn ? ui_arrow_from_turn(s->turn_code) : ARW_STRAIGHT;
            ui_arrow_set(m.turn, direction, 0xFFFFFF);
            lv_obj_clear_flag(m.turn, LV_OBJ_FLAG_HIDDEN);
            fmt_dist(text, sizeof(text), s->distance_m);
            ui_set_text(m.turn_distance, text);
            if (text[0]) lv_obj_clear_flag(m.turn_distance, LV_OBJ_FLAG_HIDDEN);
            else lv_obj_add_flag(m.turn_distance, LV_OBJ_FLAG_HIDDEN);
            if (roundabout && s->roundabout_exit > 0) {
                snprintf(text, sizeof(text), "%d", s->roundabout_exit);
                ui_set_text(m.roundabout_exit, text);
                lv_obj_clear_flag(m.roundabout_exit, LV_OBJ_FLAG_HIDDEN);
            } else {
                lv_obj_add_flag(m.roundabout_exit, LV_OBJ_FLAG_HIDDEN);
            }
        } else {
            lv_obj_add_flag(m.turn, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(m.turn_distance, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(m.roundabout_exit, LV_OBJ_FLAG_HIDDEN);
        }

        distance[0] = '\0';
        duration[0] = '\0';
        if (s->has_remaining) fmt_dist(distance, sizeof(distance), s->remaining_m);
        if (s->remaining_min >= 0) fmt_duration(duration, sizeof(duration), s->remaining_min);
        if (distance[0] && s->eta[0] && duration[0])
            snprintf(text, sizeof(text), "%s  •  %s %s", distance, s->eta, duration);
        else if (distance[0] && s->eta[0])
            snprintf(text, sizeof(text), "%s  •  %s", distance, s->eta);
        else if (distance[0])
            snprintf(text, sizeof(text), "%s", distance);
        else if (s->eta[0] && duration[0])
            snprintf(text, sizeof(text), "%s %s", s->eta, duration);
        else if (s->eta[0])
            snprintf(text, sizeof(text), "%s", s->eta);
        else
            text[0] = '\0';
        ui_set_text(m.route, text);

        if (s->progress_percent >= 0 && s->progress_percent <= 100) {
            lv_bar_set_value(m.progress, 100 - s->progress_percent, LV_ANIM_OFF);
            lv_obj_clear_flag(m.progress, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(m.progress, LV_OBJ_FLAG_HIDDEN);
        }
    } else {
        lv_obj_add_flag(m.turn, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(m.turn_distance, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(m.roundabout_exit, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(m.progress, LV_OBJ_FLAG_HIDDEN);
        ui_set_text(m.route, "");
    }

    hud_alert_t alerts[HUD_MAX_ALERTS];
    int alert_count = sn->fresh ? hud_alerts_get(alerts) : 0;
    if (sn->fresh && s->avg_zone && alert_count == 0) {
        alerts[0] = (hud_alert_t){ .code = 9, .distance_m = s->avg_zone_remaining_m };
        alert_count = 1;
    }
    const int alert_code = alert_count > 0 ? alerts[0].code : 0;
    if (alert_code > 0 && alert_code < 76 && hud_alert_icons[alert_code]) {
        lv_img_set_src(m.alert, hud_alert_icons[alert_code]);
        lv_obj_clear_flag(m.alert, LV_OBJ_FLAG_HIDDEN);
        fmt_dist(text, sizeof(text), alerts[0].distance_m);
        ui_set_text(m.alert_distance, text);
        if (text[0]) lv_obj_clear_flag(m.alert_distance, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(m.alert_distance, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(m.alert, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(m.alert_distance, LV_OBJ_FLAG_HIDDEN);
    }
}
