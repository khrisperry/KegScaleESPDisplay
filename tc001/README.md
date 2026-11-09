# Keg Scale Display — ULANZI TC001

Development firmware **1.0.3-tc001-dev** receives the scale's existing **MQTT telemetry over Wi-Fi**. It shares the MQTT broker used by Home Assistant. TouchPour retains its direct Wi-Fi controller connection and the e-Ink display retains its BLE connection. The clock does not pair with the scale or consume a controller slot. No scale firmware changes or Home Assistant automations are required.

This is a separate ESP-IDF project. Existing e-paper, touchscreen and TouchPour projects are unchanged.

## What it shows

The 32×8 RGB matrix rotates every 8 seconds:

1. Beer mug + **whole servings remaining**, rounded down from the scale's value and based on its configured serving size.
2. Keg icon + **percent remaining**.
3. **US gallons remaining**, with `G` suffix.
4. Scrolling **beer name**.

The bottom fill bar is green above 25%, amber at 11–25%, red at 10% or below. `SETUP` replaces unconfigured/uncalibrated scale readings; `SETTLE` replaces unstable readings. An empty but configured keg correctly displays zero.

Open [preview/index.html](preview/index.html) in a modern browser for the standalone interactive preview. [preview/screens.png](preview/screens.png) shows example screens. Both use the actual C renderer with sample data.

## Gather the cottage MQTT settings

Open the **scale's webpage → Settings → Home Assistant** and note:

| Setting | TC001 field |
| --- | --- |
| MQTT broker URI, e.g. `mqtt://192.168.90.10:1883` | Broker `192.168.90.10`, port `1883`, TLS off |
| MQTT username and password | MQTT username and password; re-enter the password you originally configured |
| Base topic, e.g. `kegscale/ab8070` | Scale MQTT base topic, copied exactly |

Examples above are placeholders, not your actual cottage addresses or topic. The base topic uses **six** final MAC digits; do not infer it from the four digits in the scale's hostname. Do not enter a Home Assistant entity ID, discovery prefix (`homeassistant`), wildcard, or trailing `/state`.

You may use the existing MQTT login, or a separate broker account with permission to **read** the scale's `<base>/state` and `<base>/availability` topics. The clock never publishes readings, commands, or Home Assistant discovery. It uses its own `KegPixel-XXXX` client ID, distinct from the scale and Home Assistant.

## First setup

1. Flash the clock through USB. First boot opens an open setup network named **KegPixel-XXXX**.
2. The clock cycles through the network name, `192.168.4.1`, and `WIFI SETUP`. **The temporary setup network has no password.**
3. Join that network from your phone/computer. Stay connected if it says “No Internet,” and open **http://192.168.4.1/**.
4. Scan/select your cottage's **2.4 GHz Wi-Fi** and enter its Wi-Fi password.
5. Enter the cottage **MQTT broker hostname/IP, port, MQTT username/password, and scale base topic**. The broker address is not necessarily the scale or Home Assistant address; use the broker URI from the scale settings.
6. Save. The clock restarts, connects to Wi-Fi and MQTT, subscribes to the chosen scale, and waits for a fresh reading. Normally this takes up to 10–20 seconds after connecting.

**Do not remove TouchPour, open Add touchscreen, or change e-Ink pairing.** There is no verification code for this MQTT connection. Home Assistant itself does not need to forward the data. The MQTT broker must remain running and the scale must continue publishing to it.

The clock and scale need network access to the broker (normally TCP 1883); they do not need a direct connection to each other. Wi-Fi power saving is disabled for continuous USB-powered use. Broker reconnect is automatic with a 10-second retry interval; subscriptions are renewed on every connection.

### Transport and saved settings

Standard local `mqtt://` uses TCP with TLS off, generally port 1883. As with the scale's plain MQTT configuration, credentials and messages travel without transport encryption on that LAN connection.

For `mqtts://`, select TLS and enter the matching hostname/port (usually 8883). Server certificates are verified against the ESP public CA bundle, including hostname and validity checks. TLS requires network time via `pool.ntp.org`; the clock shows `TIME WAIT` until time is available. Private/self-signed CA certificates and MQTT over WebSockets are not supported in this version. Do not disable certificate verification to work around a certificate error.

Credentials persist in NVS and are never logged or returned by the setup API. Blank Wi-Fi password preserves it only for the same SSID; select **Open network** to clear it. Blank MQTT password preserves it for the same broker and username; changing either discards it unless a new password is supplied. Select **Clear saved MQTT password** to explicitly remove it.

When upgrading from the previous TC001 controller firmware, saved Wi-Fi credentials are preserved. Broker settings must be entered. Its former scale address/key are unused. If the old TC001 was paired to a scale, remove that old registration only if needed to free the slot; do not remove a current TouchPour registration.

## Freshness and status screens

The current scale publishes retained JSON snapshots every **10 seconds** to `<base>/state`, with `servings_remaining`, `remaining_percent`, `remaining_gallons`, `keg_name`, `calibrated`, and `stable`. The clock also subscribes to `<base>/availability` for `online`/`offline` notices.

The broker may deliver an old retained snapshot immediately after subscribing. Since that payload has no trustworthy timestamp, the clock **does not treat it as a live reading**. It waits for the next non-retained delivery of a new publication. Normal live subscribers receive new retained publications with the MQTT retained-delivery flag clear.

| Clock message | Meaning |
| --- | --- |
| `WIFI WAIT` | No Wi-Fi connection |
| `MQTT WAIT` | Waiting for the broker; check address, port, login, and broker permissions |
| `TIME WAIT` | TLS selected; waiting for network time |
| `NO DATA` | No live reading yet, scale reported offline, or last valid update is over 35 seconds old |
| `SETUP` | Fresh scale telemetry, but no usable configured/calibrated keg reading |
| `SETTLE` | Fresh scale telemetry, but weight is unstable |

An offline notice immediately clears the live-reading status. An `online` notice or retained state alone cannot revive it; a new valid snapshot is required. Broker/Wi-Fi disconnects also invalidate the previous reading. Malformed, oversized, out-of-range, and incomplete payloads never refresh the timeout. The 35-second window allows three normal telemetry intervals; this is a status display, not the rapid feedback source used by TouchPour.

## Buttons and recovery

| Button | Action |
| --- | --- |
| Left | Previous keg page; pause automatic rotation for 20 seconds |
| Right | Next keg page; pause automatic rotation for 20 seconds |
| Middle, short press | Cycle brightness: 6%, 13%, 25% |
| Middle, hold 5 seconds | Open the temporary, open setup network; preserve saved settings |

The setup page is accessible only through the clock's AP at `192.168.4.1`, with a per-boot setup token. On a fully configured clock, setup closes automatically after 10 minutes. With missing Wi-Fi or broker/topic settings it remains available. Saving settings restarts the clock. The setup network never requires a password, including after upgrades or recovery.

Brightness resets to 13% after restart. Battery sleep, battery telemetry, buzzer, auto brightness, and OTA are not implemented; this version is designed for continuous USB power and USB updates.

## Build and flash on Windows / VS Code

Open **ESP-IDF: Open ESP-IDF Terminal** in VS Code and use **ESP-IDF 6.0.1**. The TC001 target is classic `esp32`, not ESP32-S3/C3.

```powershell
cd C:\Users\kperry\Documents\GitHub\KegScaleESPDisplay
git fetch origin
git switch feature/tc001-pixel-display
git pull --ff-only
cd tc001
idf.py set-target esp32
idf.py build
```

Connect a USB-C **data cable**. Before replacing stock firmware, back up its original 4 MiB flash. Replace COM5 with the actual port and stop any serial monitor first:

```powershell
python -m esptool --chip esp32 --port COM5 read-flash 0x0 0x400000 "$env:USERPROFILE\Documents\tc001-original.bin"
idf.py -p COM5 flash monitor
```

If you already flashed a previous TC001 build and made a backup, keep that original backup; do not overwrite it with the development image. `Ctrl+]` exits the monitor.

If it does not enter flashing mode automatically, follow the TC001/AWTRIX flashing instructions for the actual unit. `idf.py flash` writes this project's bootloader, partition table, and application; do not flash e-paper/touchscreen/TouchPour binaries on the clock.

The firmware does not silently erase NVS on startup errors. If stock/old NVS causes `ESP_ERR_NVS_NO_FREE_PAGES` or `ESP_ERR_NVS_NEW_VERSION_FOUND`, back up as needed and run `idf.py -p COM5 erase-flash`, then flash again. This erases saved settings. A dedicated TC001 OTA hardware/channel entry would be needed before implementing OTA.

## Demo, hardware, and validation

`idf.py menuconfig` → **Keg TC001** → enable demonstration, then build/flash. It shows `DEMO` for the first 1.5 seconds of each page and does not start networking. Disable it and rebuild for live use.

Hardware mapping is grounded in AWTRIX's [TC001 docs](https://github.com/Blueforcer/awtrix3/blob/main/docs/hardware.md) and source: matrix GPIO32, 256 WS2812-compatible GRB pixels; left/middle/right buttons GPIO26/27/14, active low. Default wiring is row serpentine; menuconfig also supports four 8×8 row-progressive tiles and column serpentine. **Physical validation is still pending.** Verify orientation, color order, buttons, setup, broker authentication, live cottage updates, and Wi-Fi/broker/scale restarts on the actual unit.

```bash
# Host tests require gcc, Node, and libcjson-dev/pkg-config, or CJSON_DIR pointing
# to cJSON 1.7.19 sources (e.g. the managed component or an upstream checkout).
./tc001/tests/run.sh
python3 tc001/tools/build_preview.py  # Pillow required
```

Tests execute the actual MQTT JSON parser, freshness state and bounded fragmented-message assembler, plus the renderer and setup-page JavaScript. They cover retained-state rejection, offline/online transitions, reconnect, stale timeouts, invalid/uncalibrated readings, empty kegs, payload bounds, topic validation, settings submission and password privacy. CI builds live/demo firmware for ESP32 with ESP-IDF 6.0.1 and runs these host checks. Successful builds/tests do not establish physical operation or cottage connectivity.

Validation for this change: ESP-IDF 6.0.1 live and demo builds pass; live binary is about 1.03 MiB and fits the 2 MiB application partition. Host C tests pass with AddressSanitizer/UndefinedBehaviorSanitizer, and setup-page JavaScript tests pass. Actual TC001 and cottage-network testing remain pending.
