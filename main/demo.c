#include "hud_state.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdio.h>
#include <string.h>

/* 48 s loop: Idle -> Waze Active -> Overspeed -> Police/Jam/Hazard cards -> Lane guidance. */
static void demo_task(void *arg)
{
    (void)arg;
    hud_stats_seed_demo();
    hud_state_set_ble_connected(true);
    uint32_t tick = 0;
    for (;;) {
        const uint32_t t = ((uint32_t)(esp_timer_get_time() / 1000) / 1000) % 48;
        hud_state_t s;
        memset(&s, 0, sizeof(s));
        s.distance_m = s.remaining_m = s.progress_percent = s.route_total_m = -1;
        s.remaining_min = -1;
        s.alert_distance_m = -1;
        s.alert_subtype = -1;
        s.lane_recommended = -1;
        snprintf(s.clock_text, sizeof(s.clock_text), "14:20");
        s.has_speed = true;
        s.has_limit = true;

        if (t < 8) {                                   /* idle: no navigation data */
            s.speed_kmh = 52; s.limit_kmh = 60;
        } else {
            s.navigation = true;
            s.turn_code = t < 36 ? 2 : 1;
            s.distance_m = 400 - (int)((t - 8) % 12) * 30;
            s.remaining_m = 12400; s.progress_percent = 68; s.route_total_m = 38000;
            s.remaining_min = 8;
            snprintf(s.eta, sizeof(s.eta), "14:28");
            snprintf(s.street, sizeof(s.street), "Nguy\xE1\xBB\x85n Tr\xC3\xA3i");
            s.speed_kmh = 74; s.limit_kmh = 80;
            if (t >= 20 && t < 28) s.speed_kmh = 87;   /* over the limit */
            if (t >= 28) { s.speed_kmh = 68; s.limit_kmh = 70; }
            if (t >= 28 && t < 40) {                   /* rotate so all three cards stay alive */
                switch (tick % 3) {
                case 0: s.alert_code = 1;  s.alert_subtype = 0; s.alert_distance_m = 400;  break;  /* police visible */
                case 1: s.alert_code = 6;  s.alert_subtype = 2; s.alert_distance_m = 1200; break;  /* heavy jam */
                default: s.alert_code = 14; s.alert_distance_m = 800; break;                         /* construction */
                }
            }
            if (t >= 40) {
                s.lane_count = 5; s.lane_recommended = 1;
                const uint8_t l[5] = { HUD_LANE_LEFT, HUD_LANE_STRAIGHT, HUD_LANE_SLIGHT_LEFT, HUD_LANE_STRAIGHT, HUD_LANE_RIGHT };
                memcpy(s.lanes, l, sizeof(l));
                s.remaining_m = 8400; s.progress_percent = 78; s.distance_m = 250;
                s.remaining_min = 32;
                snprintf(s.eta, sizeof(s.eta), "14:52");
            }
        }
        hud_state_update(&s);
        ++tick;
        vTaskDelay(pdMS_TO_TICKS(250));
    }
}

void hud_demo_start(void) { xTaskCreate(demo_task, "hud_demo", 4096, NULL, 3, NULL); }
