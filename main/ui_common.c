#include "ui_common.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include <math.h>
#include <string.h>

extern const uint8_t _binary_Montserrat_Medium_ttf_start[];
extern const uint8_t _binary_Montserrat_Medium_ttf_end[];

lv_font_t *ui_f_hero, *ui_f_idle, *ui_f_big, *ui_f_value, *ui_f_limit, *ui_f_road, *ui_f_title, *ui_f_body, *ui_f_small;

/* ------------------------------------------------------------------ theme */
static const ui_palette_t PAL_DARK = {
    .bg = 0x070B10, .card = 0x111923, .border = 0x26303A, .text = 0xF5F7FA, .dim = 0x83909D,
    .accent = 0x168BFF, .ok = 0x50D6A0, .warn = 0xFFB84D, .danger = 0xFF3B30,
    .police = 0x58A6FF, .jam = { 0xFFC864, 0xFFA03C, 0xFF643C, 0xFF3C3C }, .hazard = 0xFFC83C,
};
static const ui_palette_t PAL_SPORT = {
    .bg = 0x0A0908, .card = 0x1A1511, .border = 0x3A2E24, .text = 0xFFF6EA, .dim = 0x9C8C7C,
    .accent = 0xFF8A1E, .ok = 0x50D6A0, .warn = 0xFFD24D, .danger = 0xFF3B30,
    .police = 0x58A6FF, .jam = { 0xFFC864, 0xFFA03C, 0xFF643C, 0xFF3C3C }, .hazard = 0xFFC83C,
};
const ui_palette_t *ui_pal = &PAL_DARK;
static uint8_t cur_lang;

void ui_theme_apply(uint8_t theme, uint8_t lang)
{
    ui_pal = theme ? &PAL_SPORT : &PAL_DARK;
    cur_lang = lang ? 1 : 0;
}

/* ------------------------------------------------------------------ fonts */
static lv_font_t *make_font(int px, int glyphs)
{
    const size_t ttf_size = (size_t)(_binary_Montserrat_Medium_ttf_end - _binary_Montserrat_Medium_ttf_start);
    /* cache_size is in BYTES; Tiny TTF budgets font_size^2 bytes per glyph. */
    lv_font_t *f = lv_tiny_ttf_create_data_ex(_binary_Montserrat_Medium_ttf_start, ttf_size, px, (size_t)px * px * glyphs);
    configASSERT(f);
    return f;
}

void ui_common_init(void)
{
    if (ui_f_hero) return;
    ui_f_idle  = make_font(240, 12);   /* digits + '-' */
    ui_f_hero  = make_font(132, 12);
    ui_f_big   = make_font(52, 24);
    ui_f_value = make_font(38, 40);
    ui_f_limit = make_font(64, 12);
    ui_f_road  = make_font(32, 64);
    ui_f_title = make_font(28, 96);
    ui_f_body  = make_font(20, 128);
    ui_f_small = make_font(15, 128);
}

/* ------------------------------------------------------------------ strings */
static const char *const STR[S__COUNT][2] = {
    [S_WAIT_DATA]       = { "Đang chờ dữ liệu Waze", "Waiting for Waze data" },
    [S_BLE_OFF]         = { "BLE  •  CHƯA KẾT NỐI", "BLE  •  NOT CONNECTED" },
    [S_BLE_WAIT]        = { "BLE  •  ĐÃ KẾT NỐI · CHỜ DỮ LIỆU", "BLE  •  CONNECTED · WAITING" },
    [S_LIVE]            = { "WAZE  •  TRỰC TIẾP", "WAZE  •  LIVE" },
    [S_DEMO]            = { "DEMO", "DEMO" },
    [S_PREVIEW]         = { "XEM TRƯỚC  •  CHƯA KẾT NỐI", "PREVIEW  •  NOT CONNECTED" },
    [S_REMAINING]       = { "Còn lại", "Remaining" },
    [S_ETA]             = { "ETA", "ETA" },
    [S_OVERSPEED]       = { "QUÁ TỐC ĐỘ", "OVER SPEED" },
    [S_KMH]             = { "km/h", "km/h" },
    [S_POLICE]          = { "CẢNH SÁT", "POLICE" },
    [S_POLICE_VISIBLE]  = { "Hiện diện", "Visible" },
    [S_POLICE_HIDING]   = { "Ẩn", "Hidden" },
    [S_POLICE_CAMERA]   = { "Camera", "Camera" },
    [S_JAM]             = { "KẸT XE", "TRAFFIC JAM" },
    [S_JAM_LIGHT]       = { "Nhẹ", "Light" },
    [S_JAM_MODERATE]    = { "Vừa", "Moderate" },
    [S_JAM_HEAVY]       = { "Nặng", "Heavy" },
    [S_JAM_STANDSTILL]  = { "Ùn tắc", "Standstill" },
    [S_ACCIDENT]        = { "TAI NẠN", "ACCIDENT" },
    [S_HAZARD]          = { "NGUY HIỂM", "HAZARD" },
    [S_ROAD_CLOSED]     = { "ĐƯỜNG ĐÓNG", "ROAD CLOSED" },
    [S_STATS]           = { "THỐNG KÊ", "STATISTICS" },
    [S_AVG]             = { "Tốc độ TB", "Average Speed" },
    [S_MAX]             = { "Tốc độ tối đa", "Max Speed" },
    [S_DRIVE_TIME]      = { "Thời gian lái", "Driving Time" },
    [S_RECENT]          = { "TỐC ĐỘ GẦN ĐÂY (km/h)", "RECENT SPEED (km/h)" },
    [S_NOW]             = { "Nay", "Now" },
    [S_SETTINGS]        = { "CÀI ĐẶT", "SETTINGS" },
    [S_BRIGHTNESS]      = { "Độ sáng", "Brightness" },
    [S_SOUND]           = { "Cảnh báo âm thanh", "Sound Alert" },
    [S_ON]              = { "BẬT", "ON" },
    [S_OFF]             = { "TẮT", "OFF" },
    [S_THEME]           = { "Giao diện", "Theme" },
    [S_THEME_VAL]       = { "Tối / Thể thao", "Dark / Sport" },
    [S_LANGUAGE]        = { "Ngôn ngữ", "Language" },
    [S_LANG_VAL]        = { "Tiếng Việt / English", "Tiếng Việt / English" },
    [S_OTA]             = { "Cập nhật OTA", "OTA Update" },
    [S_OTA_VAL]         = { "Chưa hỗ trợ", "Not available yet" },
    [S_AUTO_DIM_NIGHT]  = { "Ban đêm", "Night" },
    [S_LANES]           = { "LÀN ĐƯỜNG", "LANE GUIDANCE" },
    [S_RECOMMENDED]     = { "KHUYẾN NGHỊ", "RECOMMENDED" },
};
const char *ui_tr(ui_str_t id) { return (id < S__COUNT && STR[id][cur_lang]) ? STR[id][cur_lang] : ""; }

/* ------------------------------------------------------------------ helpers */
lv_obj_t *ui_label(lv_obj_t *parent, const char *text, const lv_font_t *font, uint32_t color)
{
    lv_obj_t *o = lv_label_create(parent);
    lv_label_set_text(o, text);
    lv_obj_set_style_text_font(o, font, 0);
    lv_obj_set_style_text_color(o, lv_color_hex(color), 0);
    return o;
}

lv_obj_t *ui_box(lv_obj_t *parent, int x, int y, int w, int h, uint32_t color, int radius)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_size(o, w, h);
    lv_obj_set_style_bg_color(o, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(o, radius, 0);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(o, LV_OBJ_FLAG_GESTURE_BUBBLE);
    return o;
}

bool ui_set_text(lv_obj_t *label, const char *text)
{
    if (strcmp(lv_label_get_text(label), text) == 0) return false;
    lv_label_set_text(label, text);
    return true;
}

void ui_page_clean(lv_obj_t *o)
{
    lv_obj_remove_style_all(o);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(o, LV_OBJ_FLAG_GESTURE_BUBBLE);
}

/* ------------------------------------------------------------------ vector icons */
typedef struct { ui_icon_t kind; uint32_t color, bg; } icon_data_t;

static void circle(lv_draw_ctx_t *ctx, int cx, int cy, int r, uint32_t color, int border, uint32_t fill, bool filled)
{
    lv_draw_rect_dsc_t d;
    lv_draw_rect_dsc_init(&d);
    d.radius = LV_RADIUS_CIRCLE;
    d.bg_opa = filled ? LV_OPA_COVER : LV_OPA_TRANSP;
    d.bg_color = lv_color_hex(filled ? fill : 0);
    d.border_width = border;
    d.border_color = lv_color_hex(color);
    d.border_opa = border ? LV_OPA_COVER : LV_OPA_TRANSP;
    lv_area_t a = { cx - r, cy - r, cx + r - 1, cy + r - 1 };
    lv_draw_rect(ctx, &d, &a);
}

static void icon_draw_cb(lv_event_t *e)
{
    lv_obj_t *o = lv_event_get_target(e);
    const icon_data_t *d = lv_obj_get_user_data(o);
    lv_draw_ctx_t *ctx = lv_event_get_draw_ctx(e);
    lv_area_t a;
    lv_obj_get_coords(o, &a);
    const int w = lv_area_get_width(&a), cx = a.x1 + w / 2, cy = a.y1 + lv_area_get_height(&a) / 2;
    switch (d->kind) {
    case UI_ICON_CLOCK: {
        circle(ctx, cx, cy, w / 2, d->color, w / 11 + 1, 0, false);
        lv_draw_line_dsc_t l;
        lv_draw_line_dsc_init(&l);
        l.color = lv_color_hex(d->color);
        l.width = w / 11 + 1;
        l.round_start = l.round_end = 1;
        lv_point_t p0 = { cx, cy }, p1 = { cx, cy - w / 4 }, p2 = { cx + w / 5, cy + w / 10 };
        lv_draw_line(ctx, &l, &p1, &p0);
        lv_draw_line(ctx, &l, &p0, &p2);
        break;
    }
    case UI_ICON_PIN: {
        const int r = w * 3 / 10, ty = a.y1 + r + 1;
        circle(ctx, cx, ty, r, d->color, 0, d->color, true);
        lv_draw_rect_dsc_t t;
        lv_draw_rect_dsc_init(&t);
        t.bg_color = lv_color_hex(d->color);
        t.bg_opa = LV_OPA_COVER;
        lv_point_t tri[3] = { { cx - r + 1, ty + r / 2 }, { cx + r - 1, ty + r / 2 }, { cx, a.y2 } };
        lv_draw_triangle(ctx, &t, tri);
        circle(ctx, cx, ty, r / 2, d->bg, 0, d->bg, true);
        break;
    }
    case UI_ICON_MOON: {
        const int r = w / 2;
        circle(ctx, cx, cy, r, d->color, 0, d->color, true);
        circle(ctx, cx + r * 2 / 5, cy - r / 4, r * 5 / 6, d->bg, 0, d->bg, true);
        break;
    }
    }
}

static void user_free_cb(lv_event_t *e)
{
    void *p = lv_obj_get_user_data(lv_event_get_target(e));
    if (p) lv_mem_free(p);
}

lv_obj_t *ui_icon(lv_obj_t *parent, ui_icon_t kind, int size, uint32_t color, uint32_t bg)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_size(o, size, size);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    icon_data_t *d = lv_mem_alloc(sizeof(*d));
    configASSERT(d);
    *d = (icon_data_t){ kind, color, bg };
    lv_obj_set_user_data(o, d);
    lv_obj_add_event_cb(o, icon_draw_cb, LV_EVENT_DRAW_MAIN, NULL);
    lv_obj_add_event_cb(o, user_free_cb, LV_EVENT_DELETE, NULL);
    return o;
}

/* ------------------------------------------------------------------ arrows */
typedef struct { ui_arrow_t shape; uint32_t color; } arrow_data_t;
typedef struct { uint8_t n; int8_t p[6][2]; } poly_t;       /* polyline in a 0..100 box; last point = base of the head */

static const poly_t POLY[] = {
    [ARW_STRAIGHT]    = { 2, { { 50, 96 }, { 50, 42 } } },
    [ARW_LEFT]        = { 3, { { 76, 96 }, { 76, 50 }, { 38, 50 } } },
    [ARW_RIGHT]       = { 3, { { 24, 96 }, { 24, 50 }, { 62, 50 } } },
    [ARW_SLIGHT_LEFT] = { 3, { { 64, 96 }, { 64, 62 }, { 42, 40 } } },
    [ARW_SLIGHT_RIGHT]= { 3, { { 36, 96 }, { 36, 62 }, { 58, 40 } } },
    [ARW_UTURN]       = { 6, { { 72, 96 }, { 72, 42 }, { 62, 26 }, { 44, 26 }, { 34, 42 }, { 34, 60 } } },
};

static void arrow_draw_cb(lv_event_t *e)
{
    lv_obj_t *o = lv_event_get_target(e);
    const arrow_data_t *d = lv_obj_get_user_data(o);
    if (!d || d->shape == ARW_NONE) return;
    lv_draw_ctx_t *ctx = lv_event_get_draw_ctx(e);
    lv_area_t a;
    lv_obj_get_coords(o, &a);
    const int w = lv_area_get_width(&a);
    const lv_color_t col = lv_color_hex(d->color);

    if (d->shape == ARW_ARRIVE) {                         /* destination: ring + dot */
        circle(ctx, a.x1 + w / 2, a.y1 + w / 2, w * 4 / 10, d->color, w / 9 + 1, 0, false);
        circle(ctx, a.x1 + w / 2, a.y1 + w / 2, w / 8, d->color, 0, d->color, true);
        return;
    }
    const poly_t *p = &POLY[d->shape];
    lv_point_t pt[6];
    for (int i = 0; i < p->n; ++i) { pt[i].x = a.x1 + p->p[i][0] * w / 100; pt[i].y = a.y1 + p->p[i][1] * w / 100; }

    lv_draw_line_dsc_t l;
    lv_draw_line_dsc_init(&l);
    l.color = col;
    l.width = w * 14 / 100 + 1;
    l.round_start = 1;
    l.round_end = 1;
    for (int i = 0; i + 1 < p->n; ++i) lv_draw_line(ctx, &l, &pt[i], &pt[i + 1]);

    /* arrow head along the last segment */
    const float dx = (float)(pt[p->n - 1].x - pt[p->n - 2].x), dy = (float)(pt[p->n - 1].y - pt[p->n - 2].y);
    float len = sqrtf(dx * dx + dy * dy);
    if (len < 1.0f) len = 1.0f;
    const float ux = dx / len, uy = dy / len;
    const float hl = w * 0.27f, hw = w * 0.24f;
    const lv_point_t base = pt[p->n - 1];
    lv_point_t tri[3] = {
        { (lv_coord_t)(base.x + ux * hl), (lv_coord_t)(base.y + uy * hl) },
        { (lv_coord_t)(base.x - uy * hw), (lv_coord_t)(base.y + ux * hw) },
        { (lv_coord_t)(base.x + uy * hw), (lv_coord_t)(base.y - ux * hw) },
    };
    lv_draw_rect_dsc_t r;
    lv_draw_rect_dsc_init(&r);
    r.bg_color = col;
    r.bg_opa = LV_OPA_COVER;
    lv_draw_triangle(ctx, &r, tri);
}

lv_obj_t *ui_arrow_create(lv_obj_t *parent, int size)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_size(o, size, size);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    arrow_data_t *d = lv_mem_alloc(sizeof(*d));
    configASSERT(d);
    *d = (arrow_data_t){ ARW_NONE, 0xFFFFFF };
    lv_obj_set_user_data(o, d);
    lv_obj_add_event_cb(o, arrow_draw_cb, LV_EVENT_DRAW_MAIN, NULL);
    lv_obj_add_event_cb(o, user_free_cb, LV_EVENT_DELETE, NULL);
    return o;
}

void ui_arrow_set(lv_obj_t *o, ui_arrow_t shape, uint32_t color)
{
    arrow_data_t *d = lv_obj_get_user_data(o);
    if (d->shape == shape && d->color == color) return;
    d->shape = shape;
    d->color = color;
    lv_obj_invalidate(o);
}

/* Same grouping the original firmware used (HLP maneuver codes still need confirming on a real phone). */
ui_arrow_t ui_arrow_from_turn(int c)
{
    switch (c) {
    case 0:  return ARW_NONE;
    case 1:  return ARW_STRAIGHT;
    case 2: case 4: case 6: case 11: case 13: case 15: return ARW_LEFT;
    case 3: case 5: case 7: case 9: case 12: case 14: case 16: return ARW_RIGHT;
    case 8: case 20: return ARW_UTURN;
    case 17: return ARW_ARRIVE;
    case 19: return ARW_STRAIGHT;
    default: return ARW_STRAIGHT;
    }
}

ui_arrow_t ui_arrow_from_lane(int l)
{
    switch (l) {
    case HUD_LANE_LEFT: return ARW_LEFT;
    case HUD_LANE_RIGHT: return ARW_RIGHT;
    case HUD_LANE_SLIGHT_LEFT: return ARW_SLIGHT_LEFT;
    case HUD_LANE_SLIGHT_RIGHT: return ARW_SLIGHT_RIGHT;
    case HUD_LANE_UTURN: return ARW_UTURN;
    default: return ARW_STRAIGHT;
    }
}
