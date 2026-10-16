#include "ui_common.h"

#include <stdio.h>
#include <string.h>

/* Settings page (PDF fig 5): Brightness / Sound Alert / Theme / Language / OTA. */

static struct {
    lv_obj_t *root, *clock, *br_val, *note;
} se;

static hud_settings_t get_cfg(void)
{
    hud_snapshot_t s;
    hud_state_get(&s);
    return s.cfg;
}

static void brightness_cb(lv_event_t *e)
{
    lv_obj_t *sl = lv_event_get_target(e);
    hud_settings_t c = get_cfg();
    c.brightness = (uint8_t)lv_slider_get_value(sl);
    char t[8];
    snprintf(t, sizeof(t), "%d%%", c.brightness);
    lv_label_set_text(se.br_val, t);
    /* live preview every move, flash write only when the finger lifts */
    hud_settings_set(&c, lv_event_get_code(e) == LV_EVENT_RELEASED);
}

static void sound_cb(lv_event_t *e)
{
    lv_obj_t *sw = lv_event_get_target(e);
    hud_settings_t c = get_cfg();
    c.sound = lv_obj_has_state(sw, LV_STATE_CHECKED);
    hud_settings_set(&c, true);
    lv_obj_t *lbl = lv_event_get_user_data(e);
    lv_label_set_text(lbl, ui_tr(c.sound ? S_ON : S_OFF));
    lv_obj_set_style_text_color(lbl, lv_color_hex(c.sound ? ui_pal->ok : ui_pal->dim), 0);
    if (c.sound) audio_request_test();
}

static void theme_cb(lv_event_t *e)
{
    (void)e;
    hud_settings_t c = get_cfg();
    c.theme = c.theme ? 0 : 1;
    hud_settings_set(&c, true);
    ui_request_rebuild();
}

static void lang_cb(lv_event_t *e)
{
    (void)e;
    hud_settings_t c = get_cfg();
    c.lang = c.lang ? 0 : 1;
    hud_settings_set(&c, true);
    ui_request_rebuild();
}

static void back_cb(lv_event_t *e) { (void)e; ui_nav_goto(0); }

static lv_obj_t *row(lv_obj_t *parent, int y, uint32_t color, const char *glyph, const lv_font_t *gfont,
                     const char *title, const char *sub)
{
    const ui_palette_t *P = ui_pal;
    lv_obj_t *r = ui_box(parent, 30, y, 740, 64, P->card, 14);
    lv_obj_set_style_border_width(r, 1, 0);
    lv_obj_set_style_border_color(r, lv_color_hex(P->border), 0);
    lv_obj_t *dot = ui_box(r, 14, 12, 40, 40, color, LV_RADIUS_CIRCLE);
    lv_obj_set_style_bg_opa(dot, LV_OPA_30, 0);
    lv_obj_clear_flag(dot, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t *g = ui_label(dot, glyph, gfont, color);
    lv_obj_center(g);
    lv_obj_t *t = ui_label(r, title, ui_f_body, P->text);
    lv_obj_set_pos(t, 70, sub ? 8 : 20);
    if (sub) {
        lv_obj_t *s = ui_label(r, sub, ui_f_small, P->dim);
        lv_obj_set_pos(s, 70, 36);
    }
    return r;
}

static void chevron(lv_obj_t *r, const char *value, uint32_t color)
{
    lv_obj_t *c = ui_label(r, LV_SYMBOL_RIGHT, &lv_font_montserrat_16, ui_pal->dim);
    lv_obj_align(c, LV_ALIGN_RIGHT_MID, -18, 0);
    if (value) {
        lv_obj_t *v = ui_label(r, value, ui_f_body, color);
        lv_obj_align(v, LV_ALIGN_RIGHT_MID, -52, 0);
    }
}

lv_obj_t *ui_settings_create(lv_obj_t *parent)
{
    memset(&se, 0, sizeof(se));
    const ui_palette_t *P = ui_pal;
    const hud_settings_t c = get_cfg();

    lv_obj_t *r = se.root = lv_obj_create(parent);
    ui_page_clean(r);
    lv_obj_set_size(r, 800, 480);
    lv_obj_set_style_bg_color(r, lv_color_hex(P->bg), 0);
    lv_obj_set_style_bg_opa(r, LV_OPA_COVER, 0);

    lv_obj_t *back = ui_label(r, LV_SYMBOL_LEFT, &lv_font_montserrat_20, P->text);
    lv_obj_set_pos(back, 30, 18);
    lv_obj_add_flag(back, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(back, 24);
    lv_obj_add_event_cb(back, back_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *title = ui_label(r, ui_tr(S_SETTINGS), ui_f_title, P->text);
    lv_obj_set_pos(title, 70, 8);
    se.clock = ui_label(r, "--:--", ui_f_body, P->dim);
    lv_obj_set_pos(se.clock, 716, 16);

    /* 1. Brightness */
    lv_obj_t *r1 = row(r, 56, 0xF5B400, LV_SYMBOL_EYE_OPEN, &lv_font_montserrat_20, ui_tr(S_BRIGHTNESS), NULL);
    lv_obj_t *sl = lv_slider_create(r1);
    lv_obj_set_size(sl, 300, 8);
    lv_obj_set_pos(sl, 330, 28);
    lv_slider_set_range(sl, 10, 100);
    lv_slider_set_value(sl, c.brightness, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(sl, lv_color_hex(0x29323C), LV_PART_MAIN);
    lv_obj_set_style_bg_color(sl, lv_color_hex(0xF5B400), LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(sl, lv_color_hex(0xFFFFFF), LV_PART_KNOB);
    lv_obj_set_style_pad_all(sl, 5, LV_PART_KNOB);
    lv_obj_set_ext_click_area(sl, 14);
    lv_obj_clear_flag(sl, LV_OBJ_FLAG_GESTURE_BUBBLE);          /* dragging the knob must not change page */
    char t[8];
    snprintf(t, sizeof(t), "%d%%", c.brightness);
    se.br_val = ui_label(r1, t, ui_f_body, 0xF5B400);
    lv_obj_align(se.br_val, LV_ALIGN_RIGHT_MID, -22, 0);
    lv_obj_add_event_cb(sl, brightness_cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(sl, brightness_cb, LV_EVENT_RELEASED, NULL);

    /* 2. Sound alert */
    lv_obj_t *r2 = row(r, 126, 0x3B9CFF, LV_SYMBOL_VOLUME_MAX, &lv_font_montserrat_20, ui_tr(S_SOUND), NULL);
    lv_obj_t *sw = lv_switch_create(r2);
    lv_obj_set_size(sw, 64, 32);
    lv_obj_align(sw, LV_ALIGN_RIGHT_MID, -18, 0);
    lv_obj_set_style_bg_color(sw, lv_color_hex(0x30D158), LV_PART_INDICATOR | LV_STATE_CHECKED);
    lv_obj_clear_flag(sw, LV_OBJ_FLAG_GESTURE_BUBBLE);
    if (c.sound) lv_obj_add_state(sw, LV_STATE_CHECKED);
    lv_obj_t *onoff = ui_label(r2, ui_tr(c.sound ? S_ON : S_OFF), ui_f_body, c.sound ? P->ok : P->dim);
    lv_obj_align(onoff, LV_ALIGN_RIGHT_MID, -96, 0);
    lv_obj_add_event_cb(sw, sound_cb, LV_EVENT_VALUE_CHANGED, onoff);

    /* 3. Theme */
    lv_obj_t *r3 = row(r, 196, 0xB06BFF, LV_SYMBOL_IMAGE, &lv_font_montserrat_20, ui_tr(S_THEME), ui_tr(S_THEME_VAL));
    static const char *const THEME[2][2] = { { "Tối", "Thể thao" }, { "Dark", "Sport" } };
    chevron(r3, THEME[c.lang][c.theme], P->accent);
    lv_obj_add_event_cb(r3, theme_cb, LV_EVENT_CLICKED, NULL);

    /* 4. Language */
    lv_obj_t *r4 = row(r, 266, 0x30D1A0, "Aa", &lv_font_montserrat_16, ui_tr(S_LANGUAGE), ui_tr(S_LANG_VAL));
    chevron(r4, c.lang ? "English" : "Tiếng Việt", P->accent);
    lv_obj_add_event_cb(r4, lang_cb, LV_EVENT_CLICKED, NULL);

    /* 5. OTA (not implemented yet: shown, but inert) */
    lv_obj_t *r5 = row(r, 336, 0x22D3EE, LV_SYMBOL_UPLOAD, &lv_font_montserrat_20, ui_tr(S_OTA), ui_tr(S_OTA_VAL));
    chevron(r5, NULL, 0);

    se.note = ui_label(r, "", ui_f_small, P->dim);
    lv_obj_set_pos(se.note, 34, 414);
    return r;
}

void ui_settings_update(const hud_snapshot_t *sn)
{
    ui_set_text(se.clock, sn->s.clock_text[0] ? sn->s.clock_text : "--:--");
    char t[64];
    if (sn->night && sn->cfg.auto_dim && sn->cfg.brightness > 20)
        snprintf(t, sizeof(t), "%s: %d%%", ui_tr(S_AUTO_DIM_NIGHT), sn->brightness_pct);
    else t[0] = 0;
    ui_set_text(se.note, t);
}
