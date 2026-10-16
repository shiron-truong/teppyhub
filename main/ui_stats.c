#include "ui_common.h"

#include <stdio.h>
#include <string.h>

/* Statistics page (PDF fig 4): Average / Max / Driving time + RECENT SPEED chart (last 30 min). */

static struct {
    lv_obj_t *root, *clock;
    lv_obj_t *val[3], *unit[3];
    lv_obj_t *chart, *ylab[5], *xlab[7];
    lv_chart_series_t *ser;
    hud_stats_t last;
    bool have_last;
    int ymax;
} st;

#define CH_PX 64     /* chart origin inside the panel */
#define CH_W 650
#define CH_H 170

static void set_ymax(int ymax);

static lv_obj_t *stat_card(lv_obj_t *r, int x, ui_str_t caption, int idx, const char *unit)
{
    const ui_palette_t *P = ui_pal;
    lv_obj_t *c = ui_box(r, x, 58, 236, 98, P->card, 14);
    ui_label(c, ui_tr(caption), ui_f_small, P->dim);
    lv_obj_set_pos(lv_obj_get_child(c, 0), 16, 10);
    st.val[idx] = ui_label(c, "--", ui_f_big, P->text);
    lv_obj_set_pos(st.val[idx], 14, 30);
    st.unit[idx] = ui_label(c, unit, ui_f_body, P->dim);
    lv_obj_set_pos(st.unit[idx], 90, 58);
    return c;
}

lv_obj_t *ui_stats_create(lv_obj_t *parent)
{
    memset(&st, 0, sizeof(st));
    const ui_palette_t *P = ui_pal;
    lv_obj_t *r = st.root = lv_obj_create(parent);
    ui_page_clean(r);
    lv_obj_set_size(r, 800, 480);
    lv_obj_set_style_bg_color(r, lv_color_hex(P->bg), 0);
    lv_obj_set_style_bg_opa(r, LV_OPA_COVER, 0);

    lv_obj_t *title = ui_label(r, ui_tr(S_STATS), ui_f_title, P->text);
    lv_obj_set_width(title, 400);
    lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(title, 200, 10);
    st.clock = ui_label(r, "--:--", ui_f_body, P->dim);
    lv_obj_set_pos(st.clock, 716, 16);

    stat_card(r, 30, S_AVG, 0, "km/h");
    stat_card(r, 282, S_MAX, 1, "km/h");
    stat_card(r, 534, S_DRIVE_TIME, 2, "");

    lv_obj_t *panel = ui_box(r, 30, 170, 740, 270, P->card, 14);
    lv_obj_t *cap = ui_label(panel, ui_tr(S_RECENT), ui_f_small, P->dim);
    lv_obj_set_pos(cap, 16, 10);

    st.chart = lv_chart_create(panel);
    lv_obj_remove_style_all(st.chart);
    lv_obj_set_pos(st.chart, CH_PX, 36);
    lv_obj_set_size(st.chart, CH_W, CH_H);
    lv_obj_clear_flag(st.chart, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(st.chart, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_chart_set_type(st.chart, LV_CHART_TYPE_LINE);
    lv_chart_set_point_count(st.chart, HUD_HIST_N);
    lv_chart_set_div_line_count(st.chart, 3, 0);
    lv_obj_set_style_line_color(st.chart, lv_color_hex(P->border), LV_PART_MAIN);
    lv_obj_set_style_line_width(st.chart, 1, LV_PART_MAIN);
    lv_obj_set_style_border_width(st.chart, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(st.chart, lv_color_hex(P->border), LV_PART_MAIN);
    lv_obj_set_style_border_side(st.chart, LV_BORDER_SIDE_BOTTOM | LV_BORDER_SIDE_LEFT, LV_PART_MAIN);
    lv_obj_set_style_line_width(st.chart, 3, LV_PART_ITEMS);
    lv_obj_set_style_size(st.chart, 7, LV_PART_INDICATOR);
    st.ser = lv_chart_add_series(st.chart, lv_color_hex(P->accent), LV_CHART_AXIS_PRIMARY_Y);

    for (int i = 0; i < 5; ++i) {
        st.ylab[i] = ui_label(panel, "0", ui_f_small, P->dim);
        lv_obj_set_width(st.ylab[i], 36);
        lv_obj_set_style_text_align(st.ylab[i], LV_TEXT_ALIGN_RIGHT, 0);
    }
    for (int i = 0; i < 7; ++i) {
        char t[12];
        if (i == 6) snprintf(t, sizeof(t), "%s", ui_tr(S_NOW)); else snprintf(t, sizeof(t), "-%dm", (6 - i) * 5);
        st.xlab[i] = ui_label(panel, t, ui_f_small, P->dim);
        lv_obj_set_width(st.xlab[i], 50);
        lv_obj_set_style_text_align(st.xlab[i], LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_pos(st.xlab[i], CH_PX + i * (CH_W - 1) / 6 - 25, 36 + CH_H + 8);
    }
    set_ymax(120);
    return r;
}

static void set_ymax(int ymax)
{
    st.ymax = ymax;
    lv_chart_set_range(st.chart, LV_CHART_AXIS_PRIMARY_Y, 0, ymax);
    for (int i = 0; i < 5; ++i) {
        char t[12];
        snprintf(t, sizeof(t), "%d", ymax * i / 4);
        ui_set_text(st.ylab[i], t);
        lv_obj_set_pos(st.ylab[i], CH_PX - 44, 36 + CH_H - i * CH_H / 4 - 9);
    }
}

void ui_stats_update(const hud_snapshot_t *sn)
{
    ui_set_text(st.clock, sn->s.clock_text[0] ? sn->s.clock_text : "--:--");
    hud_stats_t s;
    hud_stats_get(&s);
    if (st.have_last && memcmp(&s, &st.last, sizeof(s)) == 0) return;
    st.last = s;
    st.have_last = true;

    char t[16];
    snprintf(t, sizeof(t), "%d", s.avg_kmh);
    if (ui_set_text(st.val[0], t)) { lv_obj_update_layout(st.val[0]); lv_obj_set_x(st.unit[0], 14 + lv_obj_get_width(st.val[0]) + 8); }
    snprintf(t, sizeof(t), "%d", s.max_kmh);
    if (ui_set_text(st.val[1], t)) { lv_obj_update_layout(st.val[1]); lv_obj_set_x(st.unit[1], 14 + lv_obj_get_width(st.val[1]) + 8); }
    snprintf(t, sizeof(t), "%02u:%02u", (unsigned)(s.drive_s / 3600), (unsigned)((s.drive_s / 60) % 60));
    ui_set_text(st.val[2], t);

    int peak = 0;
    for (int i = 0; i < s.hist_n; ++i) if (s.hist[i] > peak) peak = s.hist[i];
    int ymax = 120;
    while (ymax < peak) ymax += 40;
    if (ymax != st.ymax) set_ymax(ymax);

    for (int i = 0; i < HUD_HIST_N; ++i) {
        const int src = i - (HUD_HIST_N - s.hist_n);        /* newest sample sits at "Now" */
        lv_chart_set_value_by_id(st.chart, st.ser, i, src >= 0 ? s.hist[src] : LV_CHART_POINT_NONE);
    }
    lv_chart_refresh(st.chart);
}
