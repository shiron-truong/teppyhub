idf_component_register(
    SRCS "main.c" "hud_state.c" "audio.c" "demo.c"
         "hlp_protocol.c" "hlp_ble.c"
         "hud_ui.c" "ui_common.c" "ui_hud.c" "ui_hud_mirror.c" "ui_stats.c" "ui_settings.c"
         "alert_icons.c" "turn_icons.c"
    EMBED_FILES "fonts/Montserrat-Medium.ttf"
    INCLUDE_DIRS "."
    REQUIRES bt nvs_flash json lvgl waveshare_esp32_s3_touch_lcd_4_3
             esp_lcd esp_timer esp_driver_i2s esp_driver_gpio
)
