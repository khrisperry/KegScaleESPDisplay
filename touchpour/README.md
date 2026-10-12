# TouchPour Display — V0.1.6

One-off cottage tap handle built for **Waveshare ESP32-S3-Touch-LCD-2.8**, 240×320 portrait, 16 MB flash / 8 MB PSRAM. Not the 2.8B or 2.8C board.

## Controls and layout

- A large round **POUR** button sits at the center (150 px diameter, y=108–257). Beers remaining and the serving vessel sit above it; status (y=272–289) and instructions (y=296–313) have separate rows below. There is no on-screen manual button because the display moves with the tap handle.
- Swipe left/right above the controls to cycle beers remaining, keg percentage, and minimal gallons views. Changing views is blocked during a pour.
- POUR starts after a completed button tap/release and uses the existing TapLidarMotor calibration, stable-cup detection and distance cutoff. The round button becomes red with **EMERGENCY STOP** during a pour; the status reads **Touch anywhere to stop**. The footer shows the live **LiDAR: <distance> mm** at idle and during pouring, or indicates stale/invalid readings.
- Serving artwork follows the paired scale's serving size: 12 oz can, 16 oz pint, or 20 oz Solo cup. The compact drawings reuse the touch display's tapered outline, amber fill, gray rim/seam and Solo base styling. Other serving sizes use the pint silhouette; no crowler/growler artwork is included. The fill tracks keg percentage.
- **Touch anywhere on the screen to emergency stop an active pour.** A new touch is processed before LVGL widget dispatch, so there is no need to locate the button. Multi-touch also stops. The whole contact is consumed until all fingers are released; it cannot navigate or restart pouring. A touchscreen read failure while pouring also requests a close.
- Optional physical button: short press starts/stops automatic pouring; two-second hold starts manual fill until release.
- Pour control runs locally, independently of scale/Wi-Fi availability. Keg readings use the existing scale's encrypted Wi-Fi controller connection and pairing protocol.
- Setup disables pouring. The device website retains calibration, bounded servo tests, foam recordings, on-demand diagnostics and persistent history for the last five pours. The repeating 500 ms `tap_measure` serial messages have been removed.

## V0.1.6 installed orientation and live distance

- Turned the image 180 degrees relative to V0.1.5 after physical testing found it upside down. V0.1.5 already set LVGL rotation to 180; this correction returns to rotation 0, retaining the readable panel mirror baseline. LVGL changes pointer mapping with display orientation; raw emergency-stop detection remains independent of coordinates.
- Replaced the swipe footer with the controller's current LiDAR reading in millimeters, updated every 50 ms. Invalid readings show **no reading** and measurements older than the controller's freshness limit show **stale**. The distance stays visible during pouring, with the emergency-stop reminder in the separate status row above.
- Version/build checks are local; `[skip ci]` prevents GitHub Actions for this update. Confirm the screen and touch targets in the installed handle, then verify touch-anywhere stop.

## V0.1.5 screen and speaker cues

- Cup detection: one soft chirp after the controller confirms a stable cup. Detection stays latched through a pour/stop and readiness resets; a valid out-of-range reading lasting 600 ms rearms it for the next cup.
- Pour start: two rising notes after the controller accepts the start and commands the servo open. Rejected starts are silent.
- Fill target reached: three-note completion chime. Emergency stop, cup removal, sensor faults, maximum time and manual closure use a distinct descending stop tone.
- Sounds are generated locally using the board's PCM5101 output, stereo 16-bit Philips I2S at 16 kHz: BCLK GPIO48, LRCK GPIO38, DIN GPIO47. No additional wiring beyond connecting the supplied speaker is needed; the board's physical volume knob sets loudness.
- Setup includes a persistent **Sounds: On / Muted** toggle and **Test speaker**. Audio errors are logged without interrupting pour control. A low-priority audio worker handles DMA; the controller posts cues without waiting, after sending any required close command. The newest cue replaces pending/playing audio so stop cannot wait behind a start chime.
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
cd C:\Users\kperry\Documents\GitHub\KegScaleESPDisplay
git pull
cd touchpour
idf.py set-target esp32s3
idf.py build
idf.py -p COM7 flash monitor
```

Replace COM7 with the display's port. If USB flashing does not connect, hold BOOT while tapping RESET, release BOOT, and retry. This project has its own build directory and firmware name `touchpour_display.bin`.

On first boot, enter Wi-Fi using the screen's Setup page (scan, select SSID, password, show/hide). Save the credentials; enter or discover the scale address. Open **Add touchscreen** on the scale to complete the existing pairing flow. A pairing code appears in the status line. Return to the display with the Back button.

Reopen Setup after connecting to see the device web address and QR code. Configure the servo endpoints and LiDAR distances on that website, then explicitly return to the display before testing pours. Calibration starts unset; no opening command is permitted until servo calibration is saved.

## Updates

Initial updates use USB or **Setup → Firmware** on the device website with `touchpour_display.bin` (application image only). The image validator requires ESP32-S3 and the TouchPour project identity. The existing touchscreen OTA feed is incompatible and is deliberately not enabled for TouchPour. Wi-Fi, pairing and calibration are retained on web updates. A first flash does not migrate calibration from a different ESP32 device.

## Verification

```bash
bash touchpour/tests/run_host_tests.sh
```

This includes existing automatic cutoff/button/foam checks, anywhere-touch emergency contacts and the actual pour-controller loop with timed emergency stops and fresh-contact rearming. Hardware acceptance on arrival: verify V1/V2 log, portrait orientation, touch/button alignment, Wi-Fi and scale pairing, servo endpoints with linkage disconnected, sensor readings, auto cutoff, emergency touches across the screen, held-touch restart prevention, maximum time, and persisted pour history after reboot.

Validation completed: ESP-IDF 6.0.1 ESP32-S3 build passed; application size 0x1bab20 bytes (1,813,280 bytes), within each 6 MB app slot. Six host suites passed. Physical board, sensor and servo verification is pending device arrival.
