#include "bsp/board.h"
#include "bsp/lvgl_port.h"
#include "connection_diag.h"
#include "esp_err.h"
#include "esp_log.h"
#include "hlp_ble.h"
#include "hlp_protocol.h"
#include "hud_state.h"
#include "hud_ui.h"
#include "lvgl.h"
#include "nvs_flash.h"
#include "sdkconfig.h"

static const char *TAG = "main";

void app_main(void)
{
    /* NVS: settings + BLE. */
    esp_err_t e = nvs_flash_init();
    if (e == ESP_ERR_NVS_NO_FREE_PAGES || e == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        e = nvs_flash_init();
    }
    ESP_ERROR_CHECK(e);
    connection_diag_init();

    hud_state_init();

    /* Order from the design doc: CH422G -> RGB panel -> LVGL -> GT911 -> (UI) -> backlight -> audio. */
    esp_lcd_panel_handle_t panel = NULL;
    esp_lcd_touch_handle_t touch = NULL;
    ESP_ERROR_CHECK(waveshare_esp32_s3_rgb_lcd_init(&panel, &touch));
    ESP_ERROR_CHECK(lvgl_port_init(panel, touch));
    if (lvgl_port_lock(-1)) {
        hud_ui_init(panel);
        lvgl_port_unlock();
    }
    ESP_ERROR_CHECK(waveshare_rgb_lcd_bl_on());      /* only after the first frame is ready: no garbage flash */

    hlp_protocol_init();
    hlp_ble_start();
    audio_start();
#if CONFIG_HUD_DEMO_MODE
    hud_demo_start();
    ESP_LOGW(TAG, "DEMO MODE: showing simulated data");
#endif
    ESP_LOGI(TAG, "Waze HUD ready; enable HUD Link in Waze Mod and select WazeHUD over BLE");
}
