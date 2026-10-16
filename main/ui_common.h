#pragma once
#include "hud_state.h"
#include "lvgl.h"

/* ---------- theme ---------- */
typedef struct {
    uint32_t bg, card, border, text, dim, accent, ok, warn, danger;
    uint32_t police, jam[4], hazard;
} ui_palette_t;
extern const ui_palette_t *ui_pal;
void ui_theme_apply(uint8_t theme, uint8_t lang);       /* 0 dark / 1 sport, 0 vi / 1 en */

/* ---------- fonts (Montserrat Medium via Tiny TTF: has full Vietnamese coverage) ---------- */
extern lv_font_t *ui_f_hero, *ui_f_idle, *ui_f_big, *ui_f_value, *ui_f_limit, *ui_f_road, *ui_f_title, *ui_f_body, *ui_f_small;
void ui_common_init(void);

/* ---------- strings (vi / en) ---------- */
typedef enum {
    S_WAIT_DATA, S_BLE_OFF, S_BLE_WAIT, S_LIVE, S_DEMO, S_PREVIEW, S_REMAINING, S_ETA, S_OVERSPEED, S_KMH,
    S_POLICE, S_POLICE_VISIBLE, S_POLICE_HIDING, S_POLICE_CAMERA,
    S_JAM, S_JAM_LIGHT, S_JAM_MODERATE, S_JAM_HEAVY, S_JAM_STANDSTILL,
    S_ACCIDENT, S_HAZARD, S_ROAD_CLOSED,
    S_STATS, S_AVG, S_MAX, S_DRIVE_TIME, S_RECENT, S_NOW,
    S_SETTINGS, S_BRIGHTNESS, S_SOUND, S_ON, S_OFF, S_THEME, S_THEME_VAL, S_LANGUAGE, S_LANG_VAL, S_OTA, S_OTA_VAL,
    S_AUTO_DIM_NIGHT, S_LANES, S_RECOMMENDED,
    S__COUNT
} ui_str_t;
const char *ui_tr(ui_str_t id);

/* ---------- small helpers ---------- */
lv_obj_t *ui_label(lv_obj_t *parent, const char *text, const lv_font_t *font, uint32_t color);
lv_obj_t *ui_box(lv_obj_t *parent, int x, int y, int w, int h, uint32_t color, int radius);
bool ui_set_text(lv_obj_t *label, const char *text);     /* only touches LVGL when the text changed */
void ui_page_clean(lv_obj_t *obj);                       /* no scroll, no bg, bubbles gestures */

/* ---------- vector icons ---------- */
typedef enum { UI_ICON_CLOCK, UI_ICON_PIN, UI_ICON_MOON } ui_icon_t;
lv_obj_t *ui_icon(lv_obj_t *parent, ui_icon_t kind, int size, uint32_t color, uint32_t bg);

/* ---------- turn / lane arrows (drawn, so they scale cleanly) ---------- */
typedef enum { ARW_NONE, ARW_STRAIGHT, ARW_LEFT, ARW_RIGHT, ARW_SLIGHT_LEFT, ARW_SLIGHT_RIGHT, ARW_UTURN, ARW_ARRIVE } ui_arrow_t;
lv_obj_t *ui_arrow_create(lv_obj_t *parent, int size);
void ui_arrow_set(lv_obj_t *arrow, ui_arrow_t shape, uint32_t color);
ui_arrow_t ui_arrow_from_turn(int turn_code);
ui_arrow_t ui_arrow_from_lane(int lane);

/* ---------- pages ---------- */
void ui_nav_goto(int page);          /* implemented in hud_ui.c: 0 HUD, 1 windshield HUD, 2 Stats, 3 Settings */
void ui_request_rebuild(void);       /* theme / language changed: rebuild all pages (async, safe from event cbs) */
lv_obj_t *ui_hud_create(lv_obj_t *parent);
void ui_hud_update(const hud_snapshot_t *s);
lv_obj_t *ui_hud_mirror_create(lv_obj_t *parent);
void ui_hud_mirror_update(const hud_snapshot_t *s);
lv_obj_t *ui_stats_create(lv_obj_t *parent);
void ui_stats_update(const hud_snapshot_t *s);
lv_obj_t *ui_settings_create(lv_obj_t *parent);
void ui_settings_update(const hud_snapshot_t *s);
