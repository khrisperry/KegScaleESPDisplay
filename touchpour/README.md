# TouchPour Display — V0.1.0

One-off cottage tap handle built for **Waveshare ESP32-S3-Touch-LCD-2.8**, 240×320 portrait, 16 MB flash / 8 MB PSRAM. Not the 2.8B or 2.8C board.

## Controls and layout

- One full-width **Auto Pour** button in a reserved bottom strip (y=266–309). Content and status stay above it. There is no on-screen manual button because the display moves with the tap handle.
- Swipe left/right above the controls to cycle beers remaining, keg percentage, and minimal gallons views. Changing views is blocked during a pour.
- Auto Pour starts after a completed button tap/release and uses the existing TapLidarMotor calibration, stable-cup detection and distance cutoff. It becomes **STOP** during a pour.
- **Touch anywhere on the screen to emergency stop an active pour.** A new touch is processed before LVGL widget dispatch, so there is no need to locate the button. Multi-touch also stops. The whole contact is consumed until all fingers are released; it cannot navigate or restart pouring. A touchscreen read failure while pouring also requests a close.
- Optional physical button: short press starts/stops automatic pouring; two-second hold starts manual fill until release.
- Pour control runs locally, independently of scale/Wi-Fi availability. Keg readings use the existing scale's encrypted Wi-Fi controller connection and pairing protocol.
- Setup disables pouring. The device website retains calibration, bounded servo tests, foam recordings, 500 ms serial measurements, and persistent history for the last five pours.

## Wiring

| Function | Waveshare pin | Peripheral connection |
|---|---|---|
| LiDAR SDA | GPIO11 | VL53L0X SDA |
| LiDAR SCL | GPIO10 | VL53L0X SCL |
| LiDAR power | 3V3 / GND | Existing compatible VL53L0X module VCC / GND |
| Servo control | GPIO15 | Servo signal |
| Optional physical button | GPIO18 / GND | Normally open, active-low button |
| Servo power | Separate suitable servo supply | Servo V+; common GND with Waveshare |
| USB | GPIO19 / GPIO20 | Reserved for USB flashing/logging |

The exposed I2C bus uses controller 0 and shares onboard IMU/RTC devices. The LiDAR address 0x29 does not collide with them. The touchscreen uses a **separate** controller 1 on SDA GPIO1 / SCL GPIO3, reset GPIO2 and interrupt GPIO4 (polling reads). ST7789 LCD: MOSI45, SCLK40, CS42, DC41, reset39, backlight5. Servo PWM uses LEDC timer/channel 0 at 50 Hz; backlight uses timer/channel 1 at 5 kHz.

Power the servo from a supply sized for its load and stall current; the board's 3.3 V output is for the sensor, not servo power. Tie servo supply ground to display ground. Establish the calibrated closed position before mechanically connecting the servo to the tap.

## Reused source

Scale transport, sessions, discovery, pairing guard, controller setup and text utilities are compiled directly from `../touchscreen/main`, with the shared controller_link and ble_client components. Existing touchscreen behavior is retained outside the `TOUCHPOUR_FIRMWARE` build definition. The compact portrait UI and board driver are specific to TouchPour.

Pour controller, VL53L0X driver, web UI, foam capture and history are adapted from `khrisperry/TapLidarMotor` (see `SOURCE.md` for the exact imported file hashes). The namespace prevents collisions with the scale app's settings, actions and control names. The scale connection and tap website share one Wi-Fi driver and credential store. The large foam-recording buffer uses PSRAM; pour history occupies a dedicated partition after both application slots.

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
