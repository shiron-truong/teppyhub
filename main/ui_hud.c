#include "ui_common.h"
#include "alert_icons.h"
#include "turn_icons.h"

#include <stdio.h>
#include <string.h>

/*
 * Main HUD page (800x480), following the PDF:
 *   Fig 1  Waze Active : turn arrow + distance | big speed | limit ring | street | Remaining / ETA | progress
 *   Fig 2  Overspeed   : speed turns #FF3B30 and blinks, red banner replaces the street line, sound on core 0
 *   Fig 3/7 Alerts     : one rotating warning card in the right column
 *   Fig 6  Lanes       : 3-6 lane boxes, recommended lane in accent colour
 *   Idle (no BLE / no Waze) : huge centred speed only
 */

#define COL_OVER 0xFF3B30
#define ALERT_CARD_X 608

typedef struct {
    lv_obj_t *box, *image, *sub, *sub_bold_x, *sub_bold_y, *progress;
} card_t;

static struct {
    lv_obj_t *root;
    lv_obj_t *clock, *status, *bt, *gps, *moon;
    lv_obj_t *speed, *kmh;
    lv_obj_t *turn_icon, *dist, *roundabout_exit;
    lv_obj_t *limit, *limit_lbl;
    lv_obj_t *street, *next_street, *street_divider;
    lv_obj_t *banner, *banner_lbl;
    lv_obj_t *nav_group;                     /* bottom panels + progress */
    lv_obj_t *rem, *eta, *eta_duration, *bar, *pct;
    card_t alert_card;
    lv_obj_t *lane_group, *lane_box[HUD_MAX_LANES], *lane_arrow[HUD_MAX_LANES];
} h;
static uint32_t alert_rotation_ms;
static int alert_rotation_index;
static int alert_rotation_count;
static uint32_t preview_cycle_start_ms;
static bool preview_cycle_started;
static uint32_t guidance_rotation_start_ms;
static bool guidance_rotation_active;
static const int preview_turn_codes[] = {
    1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 19, 20
};

static uint32_t ble_icon_color(const hud_snapshot_t *sn)
{
    if (!sn->s.ble_connected || !sn->has_ble_rssi) return ui_pal->dim;
    if (sn->ble_signal_pct >= 67) return ui_pal->ok;
    if (sn->ble_signal_pct >= 33) return ui_pal->warn;
    return ui_pal->danger;
}

typedef struct {
    char sub[48];
    uint32_t color;
    const lv_img_dsc_t *icon;
    int progress;
    bool show_progress;
} card_view_t;

static void fmt_dist(char *out, size_t n, int m)
{
    if (m < 0) out[0] = 0;
    else if (m >= 1000) snprintf(out, n, "%d.%d km", m / 1000, (m % 1000) / 100);
    else snprintf(out, n, "%d m", m);
}

static void fmt_duration(char *out, size_t n, int minutes)
{
    if (minutes < 0) out[0] = '\0';
    else if (minutes < 60) snprintf(out, n, "(%dp)", minutes);
    else snprintf(out, n, "(%dh%02dp)", minutes / 60, minutes % 60);
}

static void alert_view(const hud_alert_t *a, card_view_t *v)
{
    char d[16];
    fmt_dist(d, sizeof(d), a->distance_m);
    char detail[48] = "";
    v->icon = a->code > 0 && a->code < 76 && hud_alert_icons[a->code]
        ? hud_alert_icons[a->code] : hud_alert_icons[4];
    v->color = ui_pal->hazard;
    v->show_progress = false;
    switch (a->type) {
    case HUD_ALERT_POLICE:
        v->color = ui_pal->police;
        break;
    case HUD_ALERT_CAMERA:
        if (a->value > 0) snprintf(detail, sizeof(detail), "%d km/h", a->value);
        v->color = ui_pal->warn;
        break;
    case HUD_ALERT_JAM: {
        if (a->delay_min > 0) {
            snprintf(detail, sizeof(detail), "+%d min", a->delay_min);
        }
        v->color = ui_pal->jam[a->level > 3 ? 3 : a->level];
        break;
    }
    case HUD_ALERT_ACCIDENT:
        v->color = ui_pal->danger;
        break;
    case HUD_ALERT_ROAD_CLOSED:
        v->color = ui_pal->danger;
        break;
    case HUD_ALERT_AVG_ZONE:
        if (a->value > 0 && d[0])
            snprintf(detail, sizeof(detail), "%s\n%d km/h", d, a->value);
        else if (a->value > 0)
            snprintf(detail, sizeof(detail), "%d km/h", a->value);
        if (a->level > 0 && a->level <= 100) {
            v->progress = a->level;
            v->show_progress = true;
        }
        v->color = ui_pal->warn;
        break;
    default: {
        if ((a->code == 8 || a->code == 22) && a->value > 0)
            snprintf(detail, sizeof(detail), "%d km/h", a->value);
        if (a->code == 9 || a->code == 10 || (a->code >= 25 && a->code <= 39) ||
                 (a->code >= 65 && a->code <= 74)) {
            v->color = ui_pal->danger;
        }
        break;
    }
    }
    if (a->type == HUD_ALERT_AVG_ZONE && detail[0] && a->level > 0 && a->level <= 100)
        snprintf(v->sub, sizeof(v->sub), "%s\n%d%%", detail, a->level);
    else if (a->type == HUD_ALERT_AVG_ZONE && detail[0])
        snprintf(v->sub, sizeof(v->sub), "%s", detail);
    else if (a->type == HUD_ALERT_AVG_ZONE && d[0])
        snprintf(v->sub, sizeof(v->sub), "%s", d);
    else if (d[0] && detail[0]) snprintf(v->sub, sizeof(v->sub), "%s  •  %s", detail, d);
    else if (d[0]) snprintf(v->sub, sizeof(v->sub), "%s", d);
    else snprintf(v->sub, sizeof(v->sub), "%s", detail);
}

/* ------------------------------------------------------------------ build */
static void build_card(lv_obj_t *parent, card_t *c, int y)
{
    c->box = ui_box(parent, ALERT_CARD_X, y, 176, 138, ui_pal->card, 12);
    lv_obj_set_style_bg_opa(c->box, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(c->box, 0, 0);
    lv_obj_set_style_radius(c->box, 0, 0);
    c->image = lv_img_create(c->box);
    lv_obj_align(c->image, LV_ALIGN_TOP_MID, 0, 5);
    c->sub_bold_x = ui_label(c->box, "", ui_f_body, ui_pal->dim);
    lv_obj_set_pos(c->sub_bold_x, 9, 67);
    lv_label_set_long_mode(c->sub_bold_x, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(c->sub_bold_x, 158);
    lv_obj_set_height(c->sub_bold_x, 54);
    lv_obj_set_style_text_align(c->sub_bold_x, LV_TEXT_ALIGN_CENTER, 0);
    c->sub_bold_y = ui_label(c->box, "", ui_f_body, ui_pal->dim);
    lv_obj_set_pos(c->sub_bold_y, 8, 69);
    lv_label_set_long_mode(c->sub_bold_y, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(c->sub_bold_y, 158);
    lv_obj_set_height(c->sub_bold_y, 54);
    lv_obj_set_style_text_align(c->sub_bold_y, LV_TEXT_ALIGN_CENTER, 0);
    c->sub = ui_label(c->box, "", ui_f_body, ui_pal->dim);
    lv_obj_set_pos(c->sub, 8, 68);
    lv_label_set_long_mode(c->sub, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(c->sub, 158);
    lv_obj_set_height(c->sub, 54);
    lv_obj_set_style_text_align(c->sub, LV_TEXT_ALIGN_CENTER, 0);
    c->progress = lv_bar_create(c->box);
    lv_obj_set_size(c->progress, 158, 5);
    lv_obj_set_pos(c->progress, 8, 127);
    lv_bar_set_range(c->progress, 0, 100);
    lv_obj_set_style_bg_color(c->progress, lv_color_hex(ui_pal->border), LV_PART_MAIN);
    lv_obj_set_style_bg_color(c->progress, lv_color_hex(ui_pal->warn), LV_PART_INDICATOR);
    lv_obj_add_flag(c->progress, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(c->box, LV_OBJ_FLAG_HIDDEN);
}

lv_obj_t *ui_hud_create(lv_obj_t *parent)
{
    memset(&h, 0, sizeof(h));
    const ui_palette_t *P = ui_pal;
    lv_obj_t *r = h.root = lv_obj_create(parent);
    ui_page_clean(r);
    lv_obj_set_size(r, 800, 480);
    lv_obj_set_style_bg_color(r, lv_color_hex(P->bg), 0);
    lv_obj_set_style_bg_opa(r, LV_OPA_COVER, 0);

    /* top bar */
    h.clock = ui_label(r, "--:--", ui_f_title, P->text);
    lv_obj_set_pos(h.clock, 30, 20);
    h.status = ui_label(r, "", ui_f_small, P->dim);
    lv_obj_set_width(h.status, 380);
    lv_obj_set_style_text_align(h.status, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(h.status, 210, 24);
    h.bt = ui_label(r, LV_SYMBOL_BLUETOOTH, &lv_font_montserrat_20, P->dim);
    lv_obj_set_pos(h.bt, 676, 20);
    h.gps = ui_label(r, LV_SYMBOL_GPS, &lv_font_montserrat_20, P->dim);
    lv_obj_set_pos(h.gps, 716, 20);
    h.moon = ui_icon(r, UI_ICON_MOON, 22, P->text, P->bg);
    lv_obj_set_pos(h.moon, 756, 21);
    lv_obj_add_flag(h.moon, LV_OBJ_FLAG_HIDDEN);

    /* centre: speed */
    h.speed = ui_label(r, "--", ui_f_hero, P->text);
    lv_obj_set_width(h.speed, 500);
    lv_obj_set_style_text_align(h.speed, LV_TEXT_ALIGN_CENTER, 0);
    h.kmh = ui_label(r, ui_tr(S_KMH), ui_f_body, P->dim);
    lv_obj_set_width(h.kmh, 300);
    lv_obj_set_style_text_align(h.kmh, LV_TEXT_ALIGN_CENTER, 0);

    /* left: turn arrow + distance */
    h.turn_icon = lv_img_create(r);
    lv_obj_set_size(h.turn_icon, 64, 64);
    lv_obj_set_pos(h.turn_icon, 54, 70);
    lv_obj_add_flag(h.turn_icon, LV_OBJ_FLAG_HIDDEN);
    h.roundabout_exit = ui_label(r, "", ui_f_small, P->accent);
    lv_obj_set_width(h.roundabout_exit, 200);
    lv_obj_set_style_text_align(h.roundabout_exit, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(h.roundabout_exit, 0, 136);
    lv_obj_add_flag(h.roundabout_exit, LV_OBJ_FLAG_HIDDEN);
    h.dist = ui_label(r, "", ui_f_value, P->text);
    lv_obj_set_width(h.dist, 170);
    lv_obj_set_style_text_align(h.dist, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(h.dist, 20, 158);

    /* right: speed limit ring */
    h.limit = ui_box(r, 632, 65, 128, 128, 0xFFFFFF, LV_RADIUS_CIRCLE);
    lv_obj_set_style_border_width(h.limit, 9, 0);
    lv_obj_set_style_border_color(h.limit, lv_color_hex(0xE02B2B), 0);
    h.limit_lbl = ui_label(h.limit, "--", ui_f_limit, 0x000000);
    lv_obj_center(h.limit_lbl);

    /* cards */
    build_card(r, &h.alert_card, 205);

    /* street + overspeed banner share one slot */
    h.street = ui_label(r, "", ui_f_road, P->text);
    lv_obj_set_style_text_align(h.street, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(h.street, LV_LABEL_LONG_DOT);
    lv_obj_set_width(h.street, 500);
    h.next_street = ui_label(r, "", ui_f_body, P->accent);
    lv_obj_set_style_text_align(h.next_street, LV_TEXT_ALIGN_LEFT, 0);
    lv_label_set_long_mode(h.next_street, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_style_anim_speed(h.next_street, 20, LV_PART_MAIN);
    lv_obj_set_width(h.next_street, 240);
    lv_obj_add_flag(h.next_street, LV_OBJ_FLAG_HIDDEN);
    h.street_divider = lv_obj_create(r);
    lv_obj_remove_style_all(h.street_divider);
    lv_obj_set_size(h.street_divider, 40, 2);
    lv_obj_set_style_bg_color(h.street_divider, lv_color_hex(P->dim), 0);
    lv_obj_set_style_bg_opa(h.street_divider, LV_OPA_40, 0);
    lv_obj_set_pos(h.street_divider, 380, 264);
    lv_obj_add_flag(h.street_divider, LV_OBJ_FLAG_HIDDEN);
    h.banner = ui_box(r, 0, 0, 380, 46, 0x3A1210, 23);
    lv_obj_set_style_border_width(h.banner, 2, 0);
    lv_obj_set_style_border_color(h.banner, lv_color_hex(COL_OVER), 0);
    h.banner_lbl = ui_label(h.banner, "", ui_f_body, 0xFF6A60);
    lv_obj_center(h.banner_lbl);
    lv_obj_add_flag(h.banner, LV_OBJ_FLAG_HIDDEN);

    /* lane guidance */
    h.lane_group = ui_box(r, 0, 70, 320, 84, P->bg, 0);
    lv_obj_set_style_bg_opa(h.lane_group, LV_OPA_TRANSP, 0);
    for (int i = 0; i < HUD_MAX_LANES; ++i) {
        h.lane_box[i] = ui_box(h.lane_group, 0, 15, 54, 54, P->card, 10);
        lv_obj_set_style_border_width(h.lane_box[i], 2, 0);
        lv_obj_set_style_border_color(h.lane_box[i], lv_color_hex(P->border), 0);
        h.lane_arrow[i] = ui_arrow_create(h.lane_box[i], 34);
        lv_obj_center(h.lane_arrow[i]);
    }
    lv_obj_add_flag(h.lane_group, LV_OBJ_FLAG_HIDDEN);

    /* bottom: Remaining | ETA + progress */
    h.nav_group = ui_box(r, 0, 346, 800, 134, P->bg, 0);
    lv_obj_set_style_bg_opa(h.nav_group, LV_OPA_TRANSP, 0);
    lv_obj_t *p1 = ui_box(h.nav_group, 40, 30, 350, 65, P->card, 10);
    lv_obj_t *p2 = ui_box(h.nav_group, 410, 30, 350, 65, P->card, 10);
    lv_obj_t *i1 = ui_icon(p1, UI_ICON_PIN, 34, P->accent, P->card);
    lv_obj_set_pos(i1, 28, 19);
    lv_obj_t *i2 = ui_icon(p2, UI_ICON_CLOCK, 34, P->accent, P->card);
    lv_obj_set_pos(i2, 28, 19);
    h.rem = ui_label(p1, "--", ui_f_value, P->text);
    lv_obj_set_width(h.rem, 350);
    lv_obj_set_style_text_align(h.rem, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(h.rem, 0, 10);
    h.eta = ui_label(p2, "--:--", ui_f_value, P->text);
    lv_obj_set_width(h.eta, 105);
    lv_obj_set_style_text_align(h.eta, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_set_pos(h.eta, 95, 10);
    h.eta_duration = ui_label(p2, "", ui_f_title, P->text);
    lv_obj_set_width(h.eta_duration, 140);
    lv_obj_set_style_text_align(h.eta_duration, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_set_pos(h.eta_duration, 203, 15);

    h.bar = lv_slider_create(h.nav_group);                 /* slider = bar with a knob, like the mock-up */
    lv_obj_clear_flag(h.bar, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(h.bar, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_set_size(h.bar, 740, 8);
    lv_obj_set_pos(h.bar, 40, 8);
    lv_slider_set_range(h.bar, 0, 100);
    lv_obj_set_style_bg_color(h.bar, lv_color_hex(0x29323C), LV_PART_MAIN);
    lv_obj_set_style_bg_color(h.bar, lv_color_hex(P->accent), LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(h.bar, lv_color_hex(0xFFFFFF), LV_PART_KNOB);
    lv_obj_set_style_pad_all(h.bar, 4, LV_PART_KNOB);
    h.pct = ui_label(h.nav_group, "--%", ui_f_small, P->dim);
    lv_obj_set_pos(h.pct, 40, -12);
    return r;
}

/* ------------------------------------------------------------------ update */
static void place_center(lv_obj_t *obj, int cx, int y)
{
    lv_obj_set_pos(obj, cx - lv_obj_get_width(obj) / 2, y);
}

void ui_hud_update(const hud_snapshot_t *sn)
{
    hud_snapshot_t preview;
    hud_alert_t preview_alert = {0};
    int preview_alert_count = 0;
    const uint32_t now = lv_tick_get();
    if (!sn->s.ble_connected) {
        if (!preview_cycle_started) {
            preview_cycle_start_ms = now;
            preview_cycle_started = true;
        }
        const int code = (int)(((now - preview_cycle_start_ms) / 4000U) % 75U) + 1;
        preview = *sn;
        preview.fresh = true;
        preview.nav_mode = true;
        preview.over_limit = false;
        preview.over_by_kmh = 0;
        preview.s.navigation = true;
        preview.s.has_speed = true;
        preview.s.speed_kmh = 52;
        preview.s.has_limit = true;
        preview.s.limit_kmh = 60;
        preview.s.turn_code = preview_turn_codes[(now / 6000U) %
            (sizeof(preview_turn_codes) / sizeof(preview_turn_codes[0]))];
        preview.s.roundabout_exit =
            ((preview.s.turn_code >= 10 && preview.s.turn_code <= 12) ||
             preview.s.turn_code == 19 || preview.s.turn_code == 20) ? 3 : 0;
        preview.s.avg_zone = code == 9;
        preview.s.avg_zone_remaining_m = 1250;
        preview.s.avg_zone_speed_kmh = 50;
        preview.s.avg_zone_progress = 36;
        preview.s.distance_m = 120;
        preview.s.lane_count = 5;
        preview.s.lane_recommended = 1;
        preview.s.lanes[0] = HUD_LANE_LEFT;
        preview.s.lanes[1] = HUD_LANE_STRAIGHT;
        preview.s.lanes[2] = HUD_LANE_SLIGHT_LEFT;
        preview.s.lanes[3] = HUD_LANE_STRAIGHT;
        preview.s.lanes[4] = HUD_LANE_RIGHT;
        preview.s.has_remaining = true;
        preview.s.remaining_m = 7400;
        preview.s.remaining_min = 26;
        preview.s.progress_percent = 68;
        snprintf(preview.s.clock_text, sizeof(preview.s.clock_text), "09:04");
        snprintf(preview.s.eta, sizeof(preview.s.eta), "09:30");
        snprintf(preview.s.street, sizeof(preview.s.street), "Đường hiện tại");
        snprintf(preview.s.next_street, sizeof(preview.s.next_street), "Tên đường sắp tới");
        preview_alert.type = code == 1 ? HUD_ALERT_POLICE :
                             code == 2 || code == 3 || (code >= 40 && code <= 46) ? HUD_ALERT_CAMERA :
                             code == 5 ? HUD_ALERT_ACCIDENT :
                             code == 6 ? HUD_ALERT_JAM :
                             code == 7 ? HUD_ALERT_ROAD_CLOSED :
                             code == 9 ? HUD_ALERT_AVG_ZONE : HUD_ALERT_HAZARD;
        preview_alert.code = code;
        preview_alert.distance_m = 300 + ((code * 47) % 1700);
        preview_alert.level = code == 1 ? 1 : code == 6 ? 2 : code == 9 ? 36 : 0;
        preview_alert.value = code == 2 || code == 3 ? 60 :
                              code == 8 || code == 22 ? 50 :
                              code == 9 ? 50 : 0;
        preview_alert.delay_min = code == 6 ? 8 : -1;
        preview_alert_count = 1;
        sn = &preview;
    } else {
        preview_cycle_started = false;
    }
    const ui_palette_t *P = ui_pal;
    const hud_state_t *s = &sn->s;
    char t[64];

    /* alerts (also shown in Idle when Waze is driving without navigation) */
    hud_alert_t al[HUD_MAX_ALERTS + 1];
    int n_alert = preview_alert_count ? preview_alert_count : sn->fresh ? hud_alerts_get(al) : 0;
    if (preview_alert_count) al[0] = preview_alert;
    if (sn->s.ble_connected && sn->nav_mode && s->avg_zone && n_alert < HUD_MAX_ALERTS + 1) {
        al[n_alert++] = (hud_alert_t){
            .type = HUD_ALERT_AVG_ZONE,
            .code = 9,
            .level = (uint8_t)(s->avg_zone_progress >= 0 && s->avg_zone_progress <= 100
                ? s->avg_zone_progress : 0),
            .distance_m = s->avg_zone_remaining_m,
            .value = s->avg_zone_speed_kmh
        };
    }
    if (n_alert != alert_rotation_count) {
        alert_rotation_count = n_alert;
        alert_rotation_index = 0;
        alert_rotation_ms = now;
    } else if (n_alert > 1 && (uint32_t)(now - alert_rotation_ms) >= 3000) {
        alert_rotation_index = (alert_rotation_index + 1) % n_alert;
        alert_rotation_ms = now;
    }
    const bool nav = sn->nav_mode;
    const int cx = n_alert ? 280 : 400;                 /* keep street / banner clear of the card column */
    const bool blink = sn->over_limit && ((lv_tick_get() / 400) & 1);

    /* ---- top bar ---- */
    ui_set_text(h.clock, s->clock_text[0] ? s->clock_text : "--:--");
    const char *st = !s->ble_connected ? ui_tr(S_PREVIEW) : !sn->fresh ? ui_tr(S_BLE_WAIT) : ui_tr(S_LIVE);
    ui_set_text(h.status, st);
    lv_obj_set_style_text_color(h.status, lv_color_hex(!s->ble_connected ? P->dim : !sn->fresh ? P->warn : P->ok), 0);
    lv_obj_set_style_text_color(h.bt, lv_color_hex(ble_icon_color(sn)), 0);
    lv_obj_set_style_text_color(h.gps, lv_color_hex(sn->fresh && s->has_speed ? P->accent : P->dim), 0);
    if (sn->night) lv_obj_clear_flag(h.moon, LV_OBJ_FLAG_HIDDEN); else lv_obj_add_flag(h.moon, LV_OBJ_FLAG_HIDDEN);

    /* ---- speed ---- */
    if (sn->fresh && s->has_speed) snprintf(t, sizeof(t), "%d", s->speed_kmh); else snprintf(t, sizeof(t), "--");
    const bool big = !nav && n_alert == 0 && sn->fresh && s->has_speed;   /* Idle: huge centred speed */
    lv_obj_set_style_text_font(h.speed, big ? ui_f_idle : ui_f_hero, 0);
    ui_set_text(h.speed, t);
    lv_obj_set_style_text_color(h.speed, lv_color_hex(sn->over_limit ? COL_OVER : P->text), 0);
    lv_obj_set_style_text_opa(h.speed, blink ? LV_OPA_40 : LV_OPA_COVER, 0);
    const int speed_y = nav ? 48 : big ? 62 : 46;
    lv_obj_set_pos(h.speed, (800 - 500) / 2, speed_y);
    lv_obj_update_layout(h.speed);
    const int speed_center_y = speed_y + lv_obj_get_height(h.speed) / 2;
    place_center(h.kmh, 400, nav ? 206 : big ? 318 : 188);

    /* ---- speed limit ---- */
    const bool show_limit = sn->fresh && s->has_limit;
    if (show_limit) lv_obj_clear_flag(h.limit, LV_OBJ_FLAG_HIDDEN); else lv_obj_add_flag(h.limit, LV_OBJ_FLAG_HIDDEN);
    snprintf(t, sizeof(t), "%d", s->limit_kmh);
    if (ui_set_text(h.limit_lbl, t)) lv_obj_center(h.limit_lbl);
    lv_obj_set_pos(h.limit, 632, speed_center_y - 64);
    lv_obj_set_style_border_color(h.limit, lv_color_hex(sn->over_limit && blink ? 0xFF8A80 : 0xE02B2B), 0);

    const bool lanes = nav && !sn->over_limit && s->lane_count >= 1;
    const bool roundabout = nav && (s->roundabout_exit > 0 ||
        (s->turn_code >= 10 && s->turn_code <= 12) ||
        s->turn_code == 19 || s->turn_code == 20);
    const bool roundabout_turn = (s->turn_code >= 10 && s->turn_code <= 12) ||
        s->turn_code == 19 || s->turn_code == 20;
    const lv_img_dsc_t *turn_icon = s->turn_code >= 0 &&
        s->turn_code <= HUD_TURN_MAX_CODE ? hud_turn_icons[s->turn_code] : NULL;
    if (roundabout && !roundabout_turn) turn_icon = hud_turn_icons[10];
    const bool has_turn_guidance = turn_icon != NULL;
    const bool rotate_guidance = lanes && has_turn_guidance;
    bool show_lanes = lanes;
    if (rotate_guidance) {
        if (!guidance_rotation_active) {
            guidance_rotation_active = true;
            guidance_rotation_start_ms = now;
        }
        show_lanes = ((now - guidance_rotation_start_ms) % 6000U) >= 3000U;
    } else {
        guidance_rotation_active = false;
    }

    /* ---- lane guidance or turn arrow + distance (Waze Active only) ---- */
    if (nav) {
        if (show_lanes) {
            lv_obj_add_flag(h.turn_icon, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(h.roundabout_exit, LV_OBJ_FLAG_HIDDEN);
        } else if (roundabout && has_turn_guidance) {
            lv_img_set_src(h.turn_icon, turn_icon);
            lv_obj_clear_flag(h.turn_icon, LV_OBJ_FLAG_HIDDEN);
            if (s->roundabout_exit > 0) {
                snprintf(t, sizeof(t), "%d", s->roundabout_exit);
            } else {
                t[0] = '\0';
            }
            ui_set_text(h.roundabout_exit, t);
            if (s->roundabout_exit > 0) lv_obj_clear_flag(h.roundabout_exit, LV_OBJ_FLAG_HIDDEN);
            else lv_obj_add_flag(h.roundabout_exit, LV_OBJ_FLAG_HIDDEN);
        } else {
            if (has_turn_guidance) {
                lv_img_set_src(h.turn_icon, turn_icon);
                lv_obj_clear_flag(h.turn_icon, LV_OBJ_FLAG_HIDDEN);
            } else {
                lv_obj_add_flag(h.turn_icon, LV_OBJ_FLAG_HIDDEN);
            }
            lv_obj_add_flag(h.roundabout_exit, LV_OBJ_FLAG_HIDDEN);
        }
        fmt_dist(t, sizeof(t), s->distance_m);
        ui_set_text(h.dist, t[0] ? t : "--");
        lv_obj_set_pos(h.dist, 20, 158);
        lv_obj_clear_flag(h.dist, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(h.turn_icon, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(h.roundabout_exit, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(h.dist, LV_OBJ_FLAG_HIDDEN);
    }

    /* ---- street and overspeed banner share the y=252..330 slot ---- */
    const int slot_y = nav ? 280 : 392;
    if (sn->over_limit) {
        snprintf(t, sizeof(t), "%s  +%d km/h", ui_tr(S_OVERSPEED), sn->over_by_kmh);
        ui_set_text(h.banner_lbl, t);
        lv_obj_center(h.banner_lbl);
        lv_obj_clear_flag(h.banner, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_pos(h.banner, cx - 190, slot_y - 2);
        lv_obj_add_flag(h.street, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(h.banner, LV_OBJ_FLAG_HIDDEN);
        if (nav) {
            if (s->street[0]) {
                ui_set_text(h.street, s->street);
                lv_obj_clear_flag(h.street, LV_OBJ_FLAG_HIDDEN);
                lv_obj_set_pos(h.street, (800 - 500) / 2, slot_y);
            } else if (s->next_street[0]) {
                lv_obj_add_flag(h.street, LV_OBJ_FLAG_HIDDEN);
            } else {
                ui_set_text(h.street, ui_tr(S_WAIT_DATA));
                lv_obj_clear_flag(h.street, LV_OBJ_FLAG_HIDDEN);
                lv_obj_set_pos(h.street, (800 - 500) / 2, slot_y);
            }
        } else if (!sn->fresh) {
            ui_set_text(h.street, ui_tr(S_WAIT_DATA));
            lv_obj_set_style_text_color(h.street, lv_color_hex(P->dim), 0);
            lv_obj_clear_flag(h.street, LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_pos(h.street, (800 - 500) / 2, slot_y);
        } else {
            lv_obj_add_flag(h.street, LV_OBJ_FLAG_HIDDEN);
        }
        if (nav) lv_obj_set_style_text_color(h.street, lv_color_hex(P->text), 0);
    }

    if (show_lanes) {
        lv_obj_clear_flag(h.lane_group, LV_OBJ_FLAG_HIDDEN);
        const int cnt = s->lane_count > HUD_MAX_LANES ? HUD_MAX_LANES : s->lane_count;
        const int bw = cnt > 5 ? 44 : cnt > 3 ? 48 : 54;
        const int gap = cnt > 5 ? 4 : cnt > 3 ? 6 : 8;
        const int total = cnt * bw + (cnt - 1) * gap, x0 = (320 - total) / 2;
        for (int i = 0; i < HUD_MAX_LANES; ++i) {
            if (i >= cnt) { lv_obj_add_flag(h.lane_box[i], LV_OBJ_FLAG_HIDDEN); continue; }
            const bool rec = (i == s->lane_recommended);
            lv_obj_clear_flag(h.lane_box[i], LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_size(h.lane_box[i], bw, 54);
            lv_obj_set_pos(h.lane_box[i], x0 + i * (bw + gap), 15);
            lv_obj_set_style_bg_color(h.lane_box[i], lv_color_hex(rec ? 0x0B3A52 : P->card), 0);
            lv_obj_set_style_border_color(h.lane_box[i], lv_color_hex(rec ? 0x22D3EE : P->border), 0);
            ui_arrow_set(h.lane_arrow[i], ui_arrow_from_lane(s->lanes[i]), rec ? 0x22D3EE : 0x9AA5B1);
            lv_obj_center(h.lane_arrow[i]);
        }
    } else {
        lv_obj_add_flag(h.lane_group, LV_OBJ_FLAG_HIDDEN);
    }

    if (!sn->over_limit && nav && (s->turn_code != 0 || s->distance_m >= 0) &&
        s->next_street[0] &&
        (!s->street[0] || strcmp(s->street, s->next_street) != 0)) {
        ui_set_text(h.next_street, s->next_street);
        lv_obj_set_style_text_color(h.next_street, lv_color_hex(P->accent), 0);
        lv_obj_set_pos(h.next_street, 50, 208);
        lv_obj_clear_flag(h.next_street, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(h.next_street, LV_OBJ_FLAG_HIDDEN);
    }
    if (!sn->over_limit && nav) lv_obj_clear_flag(h.street_divider, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(h.street_divider, LV_OBJ_FLAG_HIDDEN);

    /* ---- Remaining / ETA / progress ---- */
    if (nav) {
        lv_obj_clear_flag(h.nav_group, LV_OBJ_FLAG_HIDDEN);
        if (s->has_remaining) fmt_dist(t, sizeof(t), s->remaining_m); else snprintf(t, sizeof(t), "--");
        ui_set_text(h.rem, t);
        ui_set_text(h.eta, s->eta[0] ? s->eta : "--:--");
        fmt_duration(t, sizeof(t), s->remaining_min);
        ui_set_text(h.eta_duration, t);
        if (t[0]) lv_obj_clear_flag(h.eta_duration, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(h.eta_duration, LV_OBJ_FLAG_HIDDEN);
        lv_slider_set_value(h.bar, s->progress_percent >= 0 ? s->progress_percent : 0, LV_ANIM_OFF);
        if (s->progress_percent >= 0) snprintf(t, sizeof(t), "%d%%", s->progress_percent); else snprintf(t, sizeof(t), "--%%");
        ui_set_text(h.pct, t);
    } else {
        lv_obj_add_flag(h.nav_group, LV_OBJ_FLAG_HIDDEN);
    }

    /* ---- single rotating alert card ---- */
    if (!n_alert) {
        lv_obj_add_flag(h.alert_card.box, LV_OBJ_FLAG_HIDDEN);
    } else {
        card_t *c = &h.alert_card;
        card_view_t v;
        alert_view(&al[alert_rotation_index], &v);
        lv_obj_clear_flag(c->box, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_pos(c->box, ALERT_CARD_X, 205);
        lv_obj_set_style_text_color(c->sub, lv_color_hex(v.color), 0);
        lv_obj_set_style_text_color(c->sub_bold_x, lv_color_hex(v.color), 0);
        lv_obj_set_style_text_color(c->sub_bold_y, lv_color_hex(v.color), 0);
        lv_img_set_src(c->image, v.icon);
        lv_obj_align(c->image, LV_ALIGN_TOP_MID, 0, 5);
        ui_set_text(c->sub, v.sub);
        ui_set_text(c->sub_bold_x, v.sub);
        ui_set_text(c->sub_bold_y, v.sub);
        if (v.show_progress) {
            lv_bar_set_value(c->progress, v.progress, LV_ANIM_OFF);
            lv_obj_clear_flag(c->progress, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(c->progress, LV_OBJ_FLAG_HIDDEN);
        }
    }
}
