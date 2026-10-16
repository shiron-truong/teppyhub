# Waze HUD — Waveshare ESP32-S3-Touch-LCD-4.3 (800×480)

Firmware: HLP/1 BLE GATT server (`WazeHUD`) + LVGL 8.4 dashboard following `DIY_Car_HUD_Complete_Package.pdf`.

## Build

```powershell
# IMPORTANT when upgrading from the old project: delete the old sdkconfig and build folder first,
# otherwise the old values (32 KB LVGL heap, 100 Hz tick...) override sdkconfig.defaults.
del sdkconfig
rmdir /s /q build
idf.py set-target esp32s3
idf.py build
idf.py -p COMx flash monitor
```
ESP-IDF 5.3+ (tested layout for 5.5.x). Then Waze Mod -> Settings -> Devices -> HUD Link -> BLE -> `WazeHUD`.

No phone? `idf.py menuconfig` -> *Waze HUD* -> enable **Feed the UI with simulated data** (48 s loop through all screens).

## Screens (swipe left/right)
1. **HUD**: Idle (huge speed) / Waze Active (when turn and lane guidance are both available, the HLP turn icon and lane diagram alternate in equal 3 s phases on a 6 s cycle, without a lane caption; roundabouts show their matching icon and exit number; turn distance remains visible; no-overtaking zones rotate through the same single warning card as other alerts; enlarged current speed is centered, with current street, speed limit, and alert nearby; remaining distance, ETA with remaining duration (`(25p)` or `(1h25p)`), and progress along the bottom) / Overspeed (red blinking speed + banner + sound) / alerts (a single fixed card rotates through up to three HLP alerts and the active no-overtaking-zone warning every 3 s, showing only the 56×56 warning icon and applicable data such as distance, speed, delay, or progress; no text label for the alert type). The HUD requests optional HLP `alrs`, uses its nearest entries, and falls back to the legacy single-alert fields when `alrs` is unavailable. When BLE is disconnected, the HUD page shows a clearly marked sample navigation preview with representative turn icons, lane guidance, a roundabout exit, a no-overtaking zone, and rotating alert samples.
2. **Windshield HUD**: swipe left from the main HUD to enter. It keeps the oversized speed, speed limit, next turn and distance, a compact active-alert icon, destination ETA/remaining distance, and route progress; secondary details such as street names and status icons are omitted. It only shows live data (no simulated preview) and explicitly indicates when Waze is disconnected or stale. The entire display is mirrored horizontally so text and turn symbols read correctly in a windshield reflection. It bypasses software dimming and uses brighter white text for reflection; this does not change brightness on other pages or the saved brightness setting.
3. **Thống kê**: average, max, driving time, 30-minute speed chart.
4. **Cài đặt**: brightness, sound, theme (Dark/Sport), language (VI/EN), OTA (placeholder). Saved in NVS.

## Structure
| File | Role |
|---|---|
| `hlp_ble.c`, `hlp_protocol.c` | BLE GATT + JSON frames (unchanged protocol) |
| `hud_state.c/.h` | model, alerts, stats, settings; UI polls snapshots (no cross-thread LVGL calls) |
| `hud_ui.c` | pages, swipe, dots, software dimming, windshield display mirroring, rebuild on theme/language |
| `ui_hud.c`, `ui_hud_mirror.c`, `ui_stats.c`, `ui_settings.c`, `ui_common.c` | screens, vector arrows/icons, fonts, strings |
| `audio.c` | MAX98357A I2S, synthesized alert tone (core 0) |
| `demo.c` | simulated data |

## Fixes vs. the previous version
- LVGL heap was 32 KB static -> now malloc (PSRAM). Tiny-TTF glyph cache was 8 KB (smaller than one 112 px glyph) -> sized per font.
- LVGL draw buffers use PSRAM to preserve internal RAM for the NimBLE host task; NimBLE task creation and BLE advertising failures are logged.
- `CONFIG_FREERTOS_HZ=100` made `pdMS_TO_TICKS(5)` = 0; now 1000. Main stack 3.5 KB -> 8 KB; protocol task 4 -> 6 KB.
- BLE thread no longer takes the LVGL mutex (`lvgl_port_lock(0)` is a non-blocking try, updates could be dropped).
- Backlight turned on only after the first UI frame; NVS initialised once in `app_main`.

## Things to verify on the real hardware
- **Dimming**: CH422G `DISP` is on/off only, so brightness/night-dimming is a black overlay (not a real backlight PWM).
- **Audio**: plays a generated two-tone alert (no MP3 decoder dependency) instead of `/over_speed.mp3`. Default pins GPIO11/12/13 are the microSD pins ("PA A"); change in menuconfig if you use microSD.
- HLP turn/alert mappings follow the [HLP/1 specification](https://wazemod.io.vn/tai-lieu/esp32/giao-thuc-hlp-v1). The preview and live HUD share one renderer; live values are populated from each HLP status snapshot. If HLP does not provide a valid route progress percentage, the HUD calculates traveled progress as `(route total distance - remaining distance) / route total distance`; it needs a positive `totalM`/`routeM`/`tdst` and a valid `rm`/`rkm` value. Lane guidance parses `lan` entries as `[directionsMask, selectedMask]`; selected lanes are highlighted and turn distance remains visible. `exit` supplies the roundabout exit number and also identifies navigation when it is the only route field present. `avg`/`avgL`/`avgR`/`avgP` show Vietnam's no-overtaking zone, remaining distance, recommended speed, and progress. HLP's `over` flag takes precedence when present; the local speed/limit threshold remains a fallback.
- Warning image mappings use HLP codes 1–75. Each incoming HLP status snapshot replaces the active alert set, so `"alrs":[]` immediately clears the card and removed alerts do not linger; when `alrs` is unavailable the single `alr` fields are used. While BLE is disconnected, the preview cycles through all 75 warning types in HLP/1 section 4.2, showing the icon and representative data for four seconds per item; code 0 (`NONE`) is not an alert and is skipped. The alert-card PNG icons are sourced from the [Waze Mod HLP/1 alert catalog](https://wazemod.io.vn/tai-lieu/esp32/giao-thuc-hlp-v1), resized and embedded as LVGL image data; no network or PNG decoder is needed at runtime.
- Turn icons follow HLP/1 section 4.1 and map `trn` codes 1–17, 19, and 20 to the catalog's corresponding Waze direction PNGs from `/assets/icons/Waze/`; duplicate source icons share one embedded bitmap. Codes 0 (`NONE`) and 18 (`FERRY`, not currently emitted) have no icon. In preview mode, all supported turn examples rotate every 6 s while lane guidance alternates with the current turn icon; live mode uses the received `trn` and `exit` values.
- Built and flashed with ESP-IDF 5.5.5. BLE advertising, Waze Mod connection, and GATT notifications were verified on the Waveshare ESP32-S3 board.
