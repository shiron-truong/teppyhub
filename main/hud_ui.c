#include "hud_state.h"
#include "hud_ui.h"
#include "ui_common.h"

#include "esp_log.h"
#include "esp_lcd_panel_ops.h"

/*
 * UI shell: 4 pages (HUD / windshield HUD / Statistics / Settings), swipe left/right, page dots, software dimming.
 * Everything here runs inside the LVGL task (timers / events) or under lvgl_port_lock() (hud_ui_init).
 */

#define PAGE_COUNT 4
static const char *TAG = "hud_ui";

static lv_obj_t *pages[PAGE_COUNT];
static lv_obj_t *dots[PAGE_COUNT];
static lv_obj_t *dim_overlay;
static lv_obj_t *transition_overlay;
static esp_lcd_panel_handle_t lcd_panel;
static lv_timer_t *mirror_switch_timer;
static int cur_page;
static int pending_page;
static bool rebuild_pending;
static int last_dim_opa = -1;
static bool panel_mirrored;

static void mirror_switch_cb(lv_timer_t *timer);
static void build(void);

static void set_page_visible(int p)
{
    cur_page = p;
    for (int i = 0; i < PAGE_COUNT; ++i) {
        if (!pages[i]) continue;
        if (i == p) lv_obj_clear_flag(pages[i], LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(pages[i], LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_bg_color(dots[i], lv_color_hex(i == p ? ui_pal->accent : 0x4A5560), 0);
        lv_obj_set_size(dots[i], i == p ? 10 : 8, i == p ? 10 : 8);
        const int x = 400 + (2 * i - (PAGE_COUNT - 1)) * 10 - 4;
        lv_obj_set_x(dots[i], panel_mirrored ? 800 - x - (i == p ? 10 : 8) : x);
    }
}

static void mirror_prepare_async(void *arg)
{
    (void)arg;
    lv_refr_now(NULL);
    mirror_switch_timer = lv_timer_create(mirror_switch_cb, 30, NULL);
    if (!mirror_switch_timer) {
        ESP_LOGE(TAG, "Failed to schedule the windshield HUD display transition");
        lv_obj_add_flag(transition_overlay, LV_OBJ_FLAG_HIDDEN);
        lv_obj_invalidate(lv_scr_act());
        lv_refr_now(NULL);
    }
}

static void mirror_switch_cb(lv_timer_t *timer)
{
    lv_timer_del(timer);
    mirror_switch_timer = NULL;
    const bool mirror = pending_page == 1;
    const esp_err_t err = esp_lcd_panel_mirror(lcd_panel, mirror, false);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to %s the windshield HUD display: %s",
                 mirror ? "mirror" : "unmirror", esp_err_to_name(err));
        lv_obj_add_flag(transition_overlay, LV_OBJ_FLAG_HIDDEN);
        lv_obj_invalidate(lv_scr_act());
        lv_refr_now(NULL);
        return;
    }
    panel_mirrored = mirror;
    set_page_visible(pending_page);
    lv_obj_add_flag(transition_overlay, LV_OBJ_FLAG_HIDDEN);
    lv_obj_invalidate(lv_scr_act());
    lv_refr_now(NULL);
}

static void show_page(int p)
{
    if (p < 0) p = 0;
    if (p >= PAGE_COUNT) p = PAGE_COUNT - 1;
    if (mirror_switch_timer) return;
    const bool mirror = p == 1;
    if (mirror != panel_mirrored) {
        pending_page = p;
        lv_obj_clear_flag(transition_overlay, LV_OBJ_FLAG_HIDDEN);
        lv_obj_invalidate(transition_overlay);
        if (lv_async_call(mirror_prepare_async, NULL) != LV_RES_OK) {
            ESP_LOGE(TAG, "Failed to schedule the windshield HUD display transition");
            lv_obj_add_flag(transition_overlay, LV_OBJ_FLAG_HIDDEN);
            lv_obj_invalidate(lv_scr_act());
        }
        return;
    }
    set_page_visible(p);
}

void ui_nav_goto(int page) { show_page(page); }

static void gesture_cb(lv_event_t *e)
{
    (void)e;
    const lv_dir_t dir = lv_indev_get_gesture_dir(lv_indev_get_act());
    if (dir == LV_DIR_LEFT) show_page(cur_page + 1);
    else if (dir == LV_DIR_RIGHT) show_page(cur_page - 1);
}

static void apply_dim(int brightness_pct)
{
    /* CH422G "DISP" is on/off only (no PWM), so dimming is done by a black overlay on the top layer. */
    int opa = (100 - brightness_pct) * 225 / 100;
    if (opa < 0) opa = 0;
    if (opa != last_dim_opa) {
        lv_obj_set_style_bg_opa(dim_overlay, (lv_opa_t)opa, 0);
        last_dim_opa = opa;
    }
}

static void refresh_cb(lv_timer_t *t)
{
    (void)t;
    hud_snapshot_t s;
    hud_state_get(&s);
    apply_dim(cur_page == 1 ? 100 : s.brightness_pct);
    switch (cur_page) {
    case 0: ui_hud_update(&s); break;
    case 1: ui_hud_mirror_update(&s); break;
    case 2: ui_stats_update(&s); break;
    default: ui_settings_update(&s); break;
    }
}

static void build(void)
{
    hud_snapshot_t s;
    hud_state_get(&s);
    ui_theme_apply(s.cfg.theme, s.cfg.lang);

    lv_obj_t *scr = lv_scr_act();
    lv_obj_clean(scr);
    lv_obj_set_style_bg_color(scr, lv_color_hex(ui_pal->bg), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    pages[0] = ui_hud_create(scr);
    pages[1] = ui_hud_mirror_create(scr);
    pages[2] = ui_stats_create(scr);
    pages[3] = ui_settings_create(scr);
    for (int i = 0; i < PAGE_COUNT; ++i) {
        const int x = 400 + (2 * i - (PAGE_COUNT - 1)) * 10 - 4;
        dots[i] = ui_box(scr, x, 466, 8, 8, 0x4A5560, LV_RADIUS_CIRCLE);
        lv_obj_clear_flag(dots[i], LV_OBJ_FLAG_CLICKABLE);
    }
    show_page(cur_page);
    refresh_cb(NULL);
}

static void rebuild_async(void *arg)
{
    (void)arg;
    rebuild_pending = false;
    build();
}

void ui_request_rebuild(void)
{
    if (rebuild_pending) return;
    rebuild_pending = true;
    lv_async_call(rebuild_async, NULL);          /* never delete widgets from inside their own event callback */
}

void hud_ui_init(esp_lcd_panel_handle_t panel)
{
    lcd_panel = panel;
    ui_common_init();
    lv_obj_t *scr = lv_scr_act();
    lv_obj_add_event_cb(scr, gesture_cb, LV_EVENT_GESTURE, NULL);

    dim_overlay = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(dim_overlay);
    lv_obj_set_size(dim_overlay, 800, 480);
    lv_obj_set_style_bg_color(dim_overlay, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(dim_overlay, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(dim_overlay, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);   /* touches pass through */

    transition_overlay = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(transition_overlay);
    lv_obj_set_size(transition_overlay, 800, 480);
    lv_obj_set_style_bg_color(transition_overlay, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(transition_overlay, LV_OPA_COVER, 0);
    lv_obj_clear_flag(transition_overlay, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(transition_overlay, LV_OBJ_FLAG_HIDDEN);

    build();
    lv_timer_create(refresh_cb, 100, NULL);
    ESP_LOGI(TAG, "UI ready: 800x480, 4 pages, Vietnamese TTF");
}
