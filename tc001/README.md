# Keg Scale Display — ULANZI TC001

First development firmware: **1.0.0-tc001-dev**, hardware ID `ulanzi_tc001`.
Separate ESP-IDF project; the existing e-paper and touchscreen targets are unchanged.

## What it shows

The 32×8 RGB matrix rotates every 8 seconds:

1. Amber beer mug and **servings remaining** (uses the serving size configured on the scale).
2. Keg icon and **percent remaining**.
3. **US gallons remaining**, with `G` suffix.
4. Scrolling **beer name**.

A bottom fill bar appears on every keg screen. Green above 25%, amber at 11–25%, red at 10% or below.
`SETUP` replaces values when the scale does not mark the keg ready; `SETTLE` replaces unstable readings.
`NO LINK` replaces values after 30 seconds without a valid snapshot. Reads are retried automatically.
Long text/numbers scroll. No temperature is shown because keg temperature is not part of the current snapshot.

Open [preview/index.html](preview/index.html) in a browser for the standalone interactive preview.
[preview/screens.png](preview/screens.png) is a screen contact sheet.
Both are generated from the real C renderer, using sample data, not a separate mock UI.

## Connection

This version connects **directly over BLE** using the existing KegScaleESP protocol and shared client.
It does not require Wi-Fi, MQTT, Home Assistant, or an Internet connection.

1. Turn on the scale and flashed clock within Bluetooth range.
2. Open the scale's webpage in your phone or computer.
3. Select the scale's **Add display** pairing flow. Only one scale should be in pairing mode.
4. The TC001 shows a fixed six-digit code. Enter it in the scale's pairing prompt.
5. After an authenticated bond and compatible state read, the clock saves the scale identity and starts showing keg data.

The one-time code is not saved. Bonds persist across reboots. Encrypted display maintenance uses the existing authenticated BLE client. Removal/replacement commands from the scale are acknowledged before deleting the local bond. The clock reports its distinct firmware version; battery voltage is reported as unavailable until the hardware divider is verified.

The scale's existing display slot limits still apply: adding this clock may replace a previously paired display. This first version does not expand scale-side display capacity, expose TC001-specific settings in the scale web UI, or apply e-paper layout settings to its local page rotation.

## Buttons

| Button | Action |
| --- | --- |
| Left | Previous page; pause automatic rotation for 20 seconds |
| Right | Next page; pause automatic rotation for 20 seconds |
| Middle, short press | Cycle brightness: 6%, 13%, 25% |
| Middle, hold 5 seconds | Forget the saved scale and return to pairing |

Brightness resets to 13% after restart. Buttons are debounced. Long-press reset may wait for an active BLE operation to finish. Battery sleep, buzzer, auto brightness, and battery percentage are not implemented; USB power is the intended initial use.

## Build and flash on Windows

Use an **ESP-IDF 5.5.2 terminal**, matching the CI toolchain. This project targets classic `esp32`, not ESP32-S3/C3.

```powershell
cd C:\Users\kperry\Documents\GitHub\KegScaleESPDisplay
git fetch origin
git switch feature/tc001-pixel-display
git pull --ff-only
cd tc001
idf.py set-target esp32
idf.py build
```

When the clock arrives, connect a USB-C **data cable**. Save the original 4 MiB flash before replacing it:

```powershell
# Replace COM5 with the actual port; stop any serial monitor first.
python -m esptool --chip esp32 --port COM5 read_flash 0x0 0x400000 tc001-original.bin
idf.py -p COM5 flash monitor
```

If it does not enter flashing mode automatically, follow the TC001/AWTRIX flashing instructions for the actual unit. Confirm successful backup before an erase is needed. `idf.py flash` uses this project's bootloader, partition table, and app; do not flash the e-paper/touchscreen images onto the clock. Replacing stock firmware also replaces its stock app functions.

First-flash NVS can contain stock data; if startup reports `ESP_ERR_NVS_NO_FREE_PAGES` or `ESP_ERR_NVS_NEW_VERSION_FOUND`, restore/back up as needed and then run `idf.py -p COM5 erase-flash` followed by `idf.py -p COM5 flash monitor`. The firmware intentionally does not automatically erase stored bonds on NVS errors.

## Demo build

`idf.py menuconfig` → **Keg TC001** → enable sample-data demonstration, then build/flash.
The demo shows `DEMO` for the first 1.5 seconds of each page and does not connect to a scale.
Disable it and rebuild to return to live mode. Demo long-press reset is ignored.

## Hardware mapping and bring-up

Hardware mapping is grounded in [AWTRIX's TC001 hardware docs](https://github.com/Blueforcer/awtrix3/blob/main/docs/hardware.md) and its [DisplayManager](https://github.com/Blueforcer/awtrix3/blob/main/src/DisplayManager.cpp)/[PeripheryManager](https://github.com/Blueforcer/awtrix3/blob/main/src/PeripheryManager.cpp) source:

| Item | Configuration |
| --- | --- |
| RGB matrix | GPIO32, WS2812-compatible GRB, 256 pixels |
| Left / middle / right | GPIO26 / GPIO27 / GPIO14, active low |
| Matrix layout | Default row serpentine; menuconfig also supports four 8×8 row-progressive tiles or column serpentine |

Physical hardware has **not** been tested. Verify matrix orientation, color order, button levels, sustained BLE reads/reconnect, pairing/removal, USB boot behavior, and power consumption when the TC001 arrives. In particular, the middle button's electrical behavior must be confirmed on the actual revision. Layout can be changed in menuconfig without changing the renderer.

## Updates and validation

**USB updates only** for this first version. It intentionally does not download/apply scale-offered e-paper firmware or reuse the e-paper/touchscreen OTA feeds. Signed TC001 OTA integration requires a dedicated hardware/channel entry and scale support in a future change.

```bash
# Host checks: mappings, thresholds, invalid floats, status screens, scrolling.
./tc001/tests/run.sh
# Regenerate standalone preview and contact sheet (Python Pillow required).
python3 tc001/tools/build_preview.py
```

The CI workflow builds both live and clearly labeled demo firmware and runs the host renderer checks. Host rendering/build success cannot establish physical pairing or matrix operation.
