#include "hud_state.h"
#include "sdkconfig.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <math.h>
#include <stdint.h>

#if CONFIG_HUD_AUDIO_ENABLE
#include "driver/i2s_std.h"

#define SAMPLE_RATE   16000
#define REPEAT_MS     6000
static const char *TAG = "audio";
static i2s_chan_handle_t tx;
static volatile bool test_request;

static void play_tone(float hz, int ms, float amp)
{
    int16_t buf[256];
    int total = SAMPLE_RATE * ms / 1000, done = 0;
    float phase = 0;
    const float step = 2.0f * (float)M_PI * hz / SAMPLE_RATE;
    while (done < total) {
        int n = total - done > 256 ? 256 : total - done;
        for (int i = 0; i < n; ++i) {
            const int k = done + i;
            float env = 1.0f;                                  /* 8 ms fade in/out -> no clicks */
            const int fade = SAMPLE_RATE / 125;
            if (k < fade) env = (float)k / fade;
            else if (k > total - fade) env = (float)(total - k) / fade;
            buf[i] = (int16_t)(sinf(phase) * amp * env * 32767.0f);
            phase += step;
        }
        size_t w = 0;
        i2s_channel_write(tx, buf, n * sizeof(int16_t), &w, 1000);
        done += n;
    }
}

static void play_silence(int ms)
{
    int16_t buf[256] = {0};
    int total = SAMPLE_RATE * ms / 1000;
    while (total > 0) {
        int n = total > 256 ? 256 : total;
        size_t w = 0;
        i2s_channel_write(tx, buf, n * sizeof(int16_t), &w, 1000);
        total -= n;
    }
}

static void play_overspeed(void)
{
    for (int i = 0; i < 3; ++i) { play_tone(1200, 130, 0.55f); play_silence(40); play_tone(900, 130, 0.55f); play_silence(120); }
}

static void audio_task(void *arg)
{
    (void)arg;
    uint32_t last_ms = 0;
    bool was_over = false;
    for (;;) {
        hud_snapshot_t s;
        hud_state_get(&s);
        const uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
        if (test_request) { test_request = false; play_overspeed(); }
        if (s.over_limit && s.cfg.sound) {
            if (!was_over || now - last_ms >= REPEAT_MS) { play_overspeed(); last_ms = (uint32_t)(esp_timer_get_time() / 1000); }
        }
        was_over = s.over_limit;
        vTaskDelay(pdMS_TO_TICKS(200));
    }
}

void audio_start(void)
{
    i2s_chan_config_t cc = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    cc.auto_clear = true;                                       /* send zeros when idle: no noise on the amp */
    esp_err_t err = i2s_new_channel(&cc, &tx, NULL);
    if (err == ESP_OK) {
        i2s_std_config_t sc = {
            .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(SAMPLE_RATE),
            .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
            .gpio_cfg = {
                .mclk = I2S_GPIO_UNUSED,
                .bclk = CONFIG_HUD_I2S_BCLK_GPIO,
                .ws = CONFIG_HUD_I2S_LRC_GPIO,
                .dout = CONFIG_HUD_I2S_DIN_GPIO,
                .din = I2S_GPIO_UNUSED,
            },
        };
        err = i2s_channel_init_std_mode(tx, &sc);
        if (err == ESP_OK) err = i2s_channel_enable(tx);
    }
    if (err != ESP_OK) { ESP_LOGE(TAG, "I2S init failed (%s); overspeed sound disabled", esp_err_to_name(err)); return; }
    /* Core 0 next to NimBLE, LVGL stays on core 1. */
    xTaskCreatePinnedToCore(audio_task, "audio", 4096, NULL, 4, NULL, 0);
    ESP_LOGI(TAG, "MAX98357A on BCLK=%d LRC=%d DIN=%d", CONFIG_HUD_I2S_BCLK_GPIO, CONFIG_HUD_I2S_LRC_GPIO, CONFIG_HUD_I2S_DIN_GPIO);
}

void audio_request_test(void) { test_request = true; }

#else
void audio_start(void) {}
void audio_request_test(void) {}
#endif
