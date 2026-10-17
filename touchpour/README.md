# TouchPour Display — V0.1.14

One-off cottage tap handle built for **Waveshare ESP32-S3-Touch-LCD-2.8**, 240×320 portrait, 16 MB flash / 8 MB PSRAM. Not the 2.8B or 2.8C board.

## Controls and layout

- A large round **POUR** button sits at the center (150 px diameter, y=108–257). Beers remaining and the serving vessel sit above it; status (y=272–289) and instructions (y=296–313) have separate rows below. There is no on-screen manual button because the display moves with the tap handle.
- Swipe left/right above the controls to cycle beers remaining, keg percentage, and minimal gallons views. Changing views is blocked during a pour.
- Beers remaining shows **complete** servings (rounded down), matching the Keg Scale website: 24.56 servings is shown as 24, not 25.
- POUR starts after a completed button tap/release and uses the existing TapLidarMotor calibration, stable-cup detection and distance cutoff. The round button becomes red with **EMERGENCY STOP** during a pour; the status reads **Touch anywhere to stop**. The footer shows the live **LiDAR: <distance> mm** at idle and during pouring, or indicates stale/invalid readings.
- Serving artwork follows the paired scale's serving size: 12 oz can, 16 oz pint, or 20 oz Solo cup. The compact images reuse the updated touchscreen artwork: reflective glass, burgundy Solo cup with ribs, brushed silver can, amber liquid and cream foam. Other serving sizes use the pint silhouette; no crowler/growler artwork is included. The fill tracks keg percentage.
- **Touch anywhere on the screen to emergency stop an active pour.** A new touch is processed before LVGL widget dispatch, so there is no need to locate the button. Multi-touch also stops. The whole contact is consumed until all fingers are released; it cannot navigate or restart pouring. A touchscreen read failure while pouring also requests a close.
- Optional physical button: short press starts/stops automatic pouring; two-second hold starts manual fill until release.
- Pour control runs locally, independently of scale/Wi-Fi availability. Keg readings use the existing scale's encrypted Wi-Fi controller connection and pairing protocol.
- Setup disables pouring. The device website retains calibration, bounded servo tests, foam recordings, on-demand diagnostics and persistent history for the last five pours. The repeating 500 ms `tap_measure` serial messages have been removed.

## V0.1.14 button state crash fix

- Fix the V0.1.13 reboot when the rendered Pour button first changes enabled/disabled state. LVGL 9.5 dereferences explicitly assigned null transition descriptors. The face now owns its styles without inheriting theme animations; no null descriptor is installed.
- Reproduced the crash with actual LVGL 9.5 on the host, then verified repeated enabled/disabled, pressed/released and blue/red state changes after rendering. The audio scheduling and redraw reductions from V0.1.13 remain.
- Full ESP-IDF build and hardware validation remain local. No erase/reset of calibration or settings is required. `[skip ci]` skips Actions.
- Optional host LVGL regression (requires CMake, a C/C++ compiler and a local LVGL 9.5.0 checkout): `cmake -S tests/lvgl -B build-host-lvgl -DLVGL_SOURCE_DIR=/path/to/lvgl`, then `cmake --build build-host-lvgl` and `ctest --test-dir build-host-lvgl --output-on-failure`.

## V0.1.13 audio scheduling and display redraws

- Stop reapplying the Pour label/font/alignment and caption visibility every 50 ms. Only actual pouring-state changes update the face. Status, LiDAR, scale name and remaining-value labels update only when their formatted text changes. The control/status polling interval is unchanged.
- Disable the theme's repeated full-face color/size animation on the SPI screen; blue/red, caption and pressed depth now change together. The dimensional gradients and artwork remain.
- Audio priority is now 6, above LVGL rendering (5) and below local pour control (8). Eight 128-frame DMA buffers provide about 23 ms of scheduling headroom instead of 12 ms. Servo-close requests never wait for playback; a brief already-queued audio tail can remain after a stop.
- Host tests cover the priority/buffer settings and real cue playback/interruption. Hardware still needs confirmation for simultaneous rendering and pouring audio; no full ESP-IDF build was available here. `[skip ci]` skips Actions.

## V0.1.12 display depth and color

- The round Pour control now has a blue vertical gradient, bright beveled rim, dark inset edge and drop shadow. Pressing it lowers the face and shortens the shadow. During pouring it uses the same finish in red; unavailable pours have a muted blue finish.
- Pint, Solo cup and can use compact 48×64 versions of the approved scale/touchscreen artwork. Fill and foam still follow keg percentage. Layers stay allocated between readings to avoid repeated object creation.
- Source artwork comes from `touchscreen/main/artwork/web_vessels.json` on the recorded display commit in `main/artwork/serving_vessels.json`. The compact pixels are checked in with checksums; the build generator uses only Python's standard library. No browser, Pillow, downloads or runtime image decoder are needed to build/render them.
- Controls, layout, whole-screen emergency stop, LiDAR smoothing and pouring sounds retain their existing behavior. Local validation only; `[skip ci]` skips GitHub Actions. Verify the new finish and text at actual screen size, disabled/pressed states and touch-anywhere stop on COM20.

## V0.1.10 web layout and idle status

- Restyled the embedded website with a consistent dark theme, clearer navigation, a Setup sidebar on desktop and wrapping navigation on phones, aligned two-column calibration fields, full-width Wi-Fi fields, grouped actions and structured pour-download rows. Kept the close control available in Setup. Everything is bundled in the firmware; no external fonts, scripts or styles are needed.
- Removed the obsolete TapLidar SoftAP password/instructions from Wi-Fi Setup; this device shares Wi-Fi with its touch display. Firmware upload now checks the correct ESP32-S3 chip ID and `touchpour_display` application name rather than rejecting the S3 image with the old C3 checks.
- Idle display status now reports the current cup/sensor condition. With no cup, it says **No cup detected** instead of retaining an earlier Emergency stop reason. An idle close request still closes the servo and clears queued starts, but records **Tap closed**. A stop during pouring or a servo test retains **Emergency stop** and the original immediate-close path.
- The red emergency button follows actual pouring/servo-test motion. Cup presence alone never activates it. Music, smoothing, calibration, history and touch-anywhere stop are retained.
- Local checks only; `[skip ci]` skips GitHub Actions. Test web calibration, network settings and upload selection, plus removing a cup while idle and stopping a real pour.

## V0.1.9 instrumental pouring tune

- Automatic and physical-button manual pours play an instrumental **99 Bottles of Beer** verse from the start, looping for as long as the controller is actually pouring. It replaces the short start cue. There are no vocals, downloads or SD-card files.
- When the controller stops pouring, the loop is replaced by the completion or descending stop cue. The servo close command still happens before the audio event is posted. Playback runs in a separate worker below pour-control priority; emergency stop remains independent of audio. The final few milliseconds already queued in I2S DMA can finish while the close command proceeds immediately.
- Setup → **Test pour tune** previews one verse and then falls silent without operating the servo. **Test speaker** retains the short completion chime. The saved sound mute and physical volume control apply to both.
- Melody-data attribution and its CC BY-SA 4.0 license are in `components/tap_control/pour_melody.h`.
- Local checks only; `[skip ci]` skips GitHub Actions. On hardware, verify music continues throughout a pour and is interrupted by target completion, touch-anywhere stop, cup removal or a sensor fault.

## V0.1.8 PCM5101 speaker clock correction

- Replaced the unsupported 16 kHz / 16-bit stereo (32 BCLK per frame) clock setup with Waveshare's 44.1 kHz / 16-bit stereo format. With no external MCLK, the PCM5101 PLL requires a supported BCLK/LRCK combination; TI's table marks 16 kHz / 32 BCLK unsupported, while 44.1 kHz / 32 BCLK is supported (1.4112 MHz BCLK). See https://www.ti.com/lit/ds/symlink/pcm5101a.pdf, BCK PLL clock table.
- Speaker-test feedback distinguishes muted sounds and an unavailable audio worker. Serial logs show the clock setup, speaker-test request and each cue played; there is no repeating audio log spam. The worker still runs below pour-control priority and never delays servo closure.
- On the ESP32-S3Touch-LCD2.8 240x320 Rev1.1 board, connect the supplied speaker to the speaker header and set the physical volume knob above minimum. In Setup, confirm **Sounds: On**, then press **Test speaker**. If silent, share the boot `tap_audio` line and logs from pressing the speaker test.
- Local build/tests only; `[skip ci]` skips GitHub Actions. Actual speaker output still needs verification on the board.

## V0.1.7 TapLidarMotor smoothing

- Imported the same three-sample median followed by an exponential moving average (alpha 0.4). Invalid readings and gaps exceeding the configured freshness limit reset the filter, so old cup measurements cannot carry into a new reading.
- The screen, web distance, and calibration captures use the smoothed value. Idle cup readiness also uses the smoothed range/spread, while retaining raw full-cup/tray checks.
- Active pour cutoff, cup removal, erratic readings, invalid/stale sensor checks and manual hold behavior retain their immediate raw inputs. Smoothing cannot delay a close command or touch-anywhere stop.
- Web status includes `raw_distance_mm`, `filtered_distance_mm`, and `filtered_valid`. Foam-test CSV keeps raw distance/delta/rate and adds filtered distance/validity columns. Persistent last-five-pours history and serial event logs retain raw readings; saved history and calibration remain compatible.
- Build and tests run locally; the commit uses `[skip ci]`. Verify a stationary cup gives a steadier display, real movement is still followed, and cutoff/anywhere-stop close promptly.

## V0.1.6 installed orientation and live distance

- Turned the image 180 degrees relative to V0.1.5 after physical testing found it upside down. V0.1.5 already set LVGL rotation to 180; this correction returns to rotation 0, retaining the readable panel mirror baseline. LVGL changes pointer mapping with display orientation; raw emergency-stop detection remains independent of coordinates.
- Replaced the swipe footer with the controller's current LiDAR reading in millimeters, updated every 50 ms. Invalid readings show **no reading** and measurements older than the controller's freshness limit show **stale**. The distance stays visible during pouring, with the emergency-stop reminder in the separate status row above.
- Version/build checks are local; `[skip ci]` prevents GitHub Actions for this update. Confirm the screen and touch targets in the installed handle, then verify touch-anywhere stop.

## V0.1.5 screen and speaker cues

- Cup detection: one soft chirp after the controller confirms a stable cup. Detection stays latched through a pour/stop and readiness resets; a valid out-of-range reading lasting 600 ms rearms it for the next cup.
- Pour start: instrumental 99 Bottles of Beer loops after the controller accepts the start and commands the servo open (V0.1.9). Rejected starts are silent.
- Fill target reached: three-note completion chime. Emergency stop, cup removal, sensor faults, maximum time and manual closure use a distinct descending stop tone.
- Sounds are generated locally using the board's PCM5101 output, stereo 16-bit Philips I2S at 44.1 kHz (corrected in V0.1.8): BCLK GPIO48, LRCK GPIO38, DIN GPIO47. No additional wiring beyond connecting the supplied speaker is needed; the board's physical volume knob sets loudness.
- Setup includes a persistent **Sounds: On / Muted** toggle and **Test speaker**. Audio errors are logged without interrupting pour control. An audio worker below pour-control priority handles DMA; the controller posts cues without waiting, after sending any required close command. The newest cue replaces pending/playing audio so stop cannot wait behind a start chime.
- Touch-anywhere emergency stop remains in the raw touch driver, before LVGL dispatch. The new screen and speaker do not alter this path.
- Built/tested locally; the update commit uses `[skip ci]` so GitHub Actions does not run for this change.

After building/flashing V0.1.5, check each of the three serving icons, swipe views, Setup and return-home spacing; test the speaker and mute persistence. Then verify stable cup → detection cue, accepted start → start cue, target stop → completion cue, and touch anywhere → immediate closure/stop tone with no restart until release and a fresh tap. Confirm cup jitter does not repeat chirps, and removing/replacing the cup rearms detection.

## Wiring

| Function | Waveshare pin | Peripheral connection |
|---|---|---|
| LiDAR SDA | GPIO11 | VL53L0X SDA |
| LiDAR SCL | GPIO10 | VL53L0X SCL |
| LiDAR power | 3V3 / GND | Existing compatible VL53L0X module VCC / GND |
| Servo control | GPIO18 | Servo signal |
| Optional physical button | GPIO15 / GND | Normally open, active-low button |
| Servo power | Separate suitable servo supply | Servo V+; common GND with Waveshare |
| USB | GPIO19 / GPIO20 | Reserved for USB flashing/logging |

The exposed I2C bus uses controller 0 and shares onboard IMU/RTC devices. The LiDAR address 0x29 does not collide with them. The touchscreen uses a **separate** controller 1 on SDA GPIO1 / SCL GPIO3, reset GPIO2 and interrupt GPIO4 (polling reads). ST7789 LCD: MOSI45, SCLK40, CS42, DC41, reset39, backlight5. Servo PWM uses LEDC timer/channel 0 at 50 Hz; backlight uses timer/channel 1 at 5 kHz.

Power the servo from a supply sized for its load and stall current; the board's 3.3 V output is for the sensor, not servo power. Tie servo supply ground to display ground. Establish the calibrated closed position before mechanically connecting the servo to the tap.

## Reused source

Scale transport, sessions, discovery, pairing guard, controller setup and text utilities are compiled directly from `../touchscreen/main`, with the shared controller_link and ble_client components. Existing touchscreen behavior is retained outside the `TOUCHPOUR_FIRMWARE` build definition. The compact portrait UI and board driver are specific to TouchPour.

Pour controller, VL53L0X driver, web UI, foam capture and history are adapted from `khrisperry/TapLidarMotor` (see `SOURCE.md` for the exact imported file hashes). The namespace prevents collisions with the scale app's settings, actions and control names. The scale connection and tap website share one Wi-Fi driver and credential store. The large foam-recording buffer uses PSRAM; pour history occupies a dedicated partition after both application slots.

## V0.1.4 quieter serial diagnostics

- Removed the unconditional 500 ms `tap_measure` serial logger task. The LiDAR/controller continues sampling and running normally; web status, optional foam capture and the last-five-pours history remain available.
- Kept event-driven servo, button, pour-stop and error logs. On each changed servo output, logs now include `Servo PWM GPIO18: <pulse> us, 50 Hz, duty=<value>/16384` so test commands can be distinguished without sensor spam.
- **Servo troubleshooting:** A steady 5 V measured across the servo's power and ground during a test is expected. Check that the signal lead really connects to GPIO18 and that the servo's power-supply ground connects to Waveshare GND. At 50 Hz a 1000–2000 us 3.3 V logic pulse averages only about 0.165–0.33 V on a DC meter. A scope/logic analyzer or servo tester is better to verify actual pulse widths. Disconnect the linkage before testing.

## V0.1.3 servo GPIO18 wiring

- Servo PWM was moved from GPIO15 to **GPIO18**, matching the original TouchPour wiring plan.
- Optional physical button moved from GPIO18 to **GPIO15** to avoid any output/input pin collision. LiDAR remains GPIO11/10.
- **Existing installations:** `touchpour/sdkconfig` is generated locally and retains the old GPIO mapping even after `git pull`; from `touchpour`, run `idf.py menuconfig` → `TouchPour hardware`, set **Servo signal GPIO = 18** and **Optional physical pour button GPIO = 15**, save, then build/flash. Compilation deliberately fails if old pin settings are left active.
- Confirm the PWM signal lead uses **GPIO18**, not VBus/3V3 or the GPIO15 expansion pin; servo power needs an external supply appropriate to the servo rating, with a common ground.
- First test: remove mechanical linkage and use the web UI's Setup → Servo test. Serial boot logs now print the selected PWM pin and 50 Hz rate.

## V0.1.1 display fixes

- Corrected a horizontal LCD mirror setting that made text appear backwards.
- Removed a second RGB565 byte swap that produced pink backgrounds and lime-green buttons instead of the configured dark theme.
- Expanded the scrollable Setup form and hid the disabled Auto Pour button while configuring the device. Home retains its dedicated Auto Pour / STOP button.
- Flash the new firmware over USB and confirm text orientation, dark colors, and touch accuracy on the physical panel. Hardware verification is still required.

## V0.1.2 orientation correction

- Fix mirrored UI text at the LVGL port layer: `lvgl_port_add_disp()` overrides `esp_lcd_panel_mirror()` calls made before display registration, so base mirror settings now live in `cfg.rotation`.
- Apply `LV_DISPLAY_ROTATION_180` for the inverted physical tap-handle mounting; portrait remains 240 × 320. LVGL rotates raw input points accordingly.
- Retain the V0.1.1 corrected RGB565 byte order and taller Setup panel.
- On hardware, confirm text is readable with the handle in its installed orientation, touch buttons and fields respond where displayed, and a touch anywhere stops an active auto pour. Verify STOP before reconnecting the servo linkage.

## Board revisions

The board driver probes CST3530 at 0x58 (V2) and CST328 at 0x1A (V1), using the register protocols in Waveshare's V2 demo. A missing touch controller keeps on-screen controls unavailable. This initial build still needs physical screen/touch orientation confirmation on the unit.

## Build and first flash (Windows PowerShell)

Use the ESP-IDF **6.0.1** shell, the same environment used for the current touchscreen.

```powershell
cd C:\Users\kperry\Documents\GitHub\KegScaleESPDisplay\touchpour
git pull --ff-only
idf.py set-target esp32s3
idf.py build
idf.py -p COM20 flash monitor
```

COM20 is the current display's port. If USB flashing does not connect, hold BOOT while tapping RESET, release BOOT, and retry. This project has its own build directory and firmware name `touchpour_display.bin`.

On first boot, enter Wi-Fi using the screen's Setup page (scan, select SSID, password, show/hide). Save the credentials; enter or discover the scale address. Open **Add touchscreen** on the scale to complete the existing pairing flow. A pairing code appears in the status line. Return to the display with the Back button.

Reopen Setup after connecting to see the device web address and QR code. Configure the servo endpoints and LiDAR distances on that website, then explicitly return to the display before testing pours. Calibration starts unset; no opening command is permitted until servo calibration is saved.

## Updates

Initial updates use USB or **Setup → Firmware** on the device website with `touchpour_display.bin` (application image only). The image validator requires ESP32-S3 and the TouchPour project identity. The existing touchscreen OTA feed is incompatible and is deliberately not enabled for TouchPour. Wi-Fi, pairing and calibration are retained on web updates. A first flash does not migrate calibration from a different ESP32 device.

## Verification

```bash
bash touchpour/tests/run_host_tests.sh
```

This includes existing automatic cutoff/button/foam checks, anywhere-touch emergency contacts and the actual pour-controller loop with timed emergency stops and fresh-contact rearming. Hardware acceptance on arrival: verify V1/V2 log, portrait orientation, touch/button alignment, Wi-Fi and scale pairing, servo endpoints with linkage disconnected, sensor readings, auto cutoff, emergency touches across the screen, held-touch restart prevention, maximum time, and persisted pour history after reboot.

Current V0.1.10 validation: ten host suites pass, including idle status and actual-controller idle-close behavior; Playwright browser checks pass for desktop/mobile layouts, aligned fields, Wi-Fi, calibration capture, pour downloads, S3 firmware validation and active-pour lockouts. The full ESP-IDF build was not rerun for this update. Browser checks can be run with `node touchpour/tests/web_test.cjs` when Playwright and its Chromium browser are installed.

Initial V0.1.0 validation: ESP-IDF 6.0.1 ESP32-S3 build passed; application size 0x1bab20 bytes (1,813,280 bytes), within each 6 MB app slot. Later V0.1.8 built successfully and the user confirmed audio output on the board.
