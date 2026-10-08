# Keg Scale Display — ULANZI TC001

Development firmware **1.0.1-tc001-dev** connects over **Wi-Fi**, using the same authenticated protocol-1 WebSocket and shared `controller_link` crypto component as the Wi-Fi touchscreen. This is a separate ESP-IDF project; the existing e-paper and touchscreen targets are unchanged.

## What it shows

The 32×8 RGB matrix rotates every 8 seconds:

1. Beer mug + **whole servings remaining**, rounded down from the scale's value and based on its configured serving size.
2. Keg icon + **percent remaining**.
3. **US gallons remaining**, with `G` suffix.
4. Scrolling **beer name**.

The bottom fill bar is green above 25%, amber at 11–25%, red at 10% or below. `SETUP` replaces invalid/not-ready readings; `SETTLE` replaces unstable readings. Readings older than 12 seconds become `NO LINK` and trigger reconnect. `WIFI WAIT` indicates Wi-Fi has disconnected.

Open [preview/index.html](preview/index.html) in a modern browser for the standalone interactive preview. [preview/screens.png](preview/screens.png) is the contact sheet. They use the actual C renderer with sample data. Keg temperature is not shown because it is not in the current scale snapshot.

## First Wi-Fi setup

1. Flash the clock through USB. First boot opens a WPA2-protected setup network named **KegPixel-XXXX**.
2. The clock cycles through its setup network name, **eight-character password**, `192.168.4.1`, and `WIFI SETUP`. The password is the eight-character screen without a label, and can contain letters and digits.
3. Join that network from your phone/computer. Stay connected if it says “No Internet,” and open **http://192.168.4.1/**.
4. Scan/select a **2.4 GHz Wi-Fi** network, enter its password, and optionally unmask it. Enter the scale's IP or hostname, for example `192.168.91.25` or `KegScale-1234.local`.
5. Save; the clock restarts and joins your network. No BLE, MQTT broker, Home Assistant, or cloud service is required.

For discovery rather than manual IP entry, first save Wi-Fi with the scale address blank. Rejoin the same setup network after restart and select **Find scales**; the clock queries the same `_kegscale._tcp` mDNS service as the touchscreen through its Wi-Fi station connection. Save the chosen scale address to restart into live mode. A manual IP works when mDNS is unavailable across VLANs. The devices need network access to each other on TCP port 80.

Wi-Fi credentials and the authorized scale key persist in NVS. A blank password preserves the existing password only for the same SSID; explicitly select **Open network** to clear it. Passwords and keys are not logged or returned by the setup API.

## Authorize the scale connection

1. Reconnect your phone/computer to your normal network and open the scale's webpage.
2. Open **Settings → Displays → Wi-Fi touchscreen** (or `/controller`). Log in there if web administrator authentication is enabled.
3. Choose **Add touchscreen**. The clock retries its connection automatically.
4. Enter the clock's **six-character hexadecimal code**, including any letters A–F, in the scale's authorization prompt.
5. After approval, the clock saves the shared key and receives live encrypted snapshots. On restart it proves possession of the saved key and reconnects automatically.

The scale currently supports **one authenticated Wi-Fi controller per scale**. The TC001 uses that existing slot, so it replaces a Wi-Fi touchscreen on the same scale; both cannot be connected concurrently. The independently paired BLE e-paper display can remain connected. This change does not alter scale-side display capacity or rename its existing touchscreen management page.

The scale pushes cached readings at roughly 2 Hz. The TC001 sends encrypted application heartbeats every 3 seconds, retries connections at 10-second intervals, and drops sessions with readings older than 12 seconds. Wi-Fi power saving is disabled for continuous USB-powered use. Unauthenticated plaintext cannot supply readings; messages are authenticated with AES-256-GCM and replay protection from the shared touchscreen component. The displayed verification code is never sent over the WebSocket or exposed in the setup API.

Removing the pairing on the scale while connected sends an encrypted `unpair` notice, which clears the clock's saved key. If removal happened while the clock was offline, use its setup page to clear its local pairing too. Before changing scales, remove the old pairing on that scale and select the local clear-pairing checkbox. Changing an already-paired host is rejected unless local pairing is explicitly cleared.

## Buttons and recovery

| Button | Action |
| --- | --- |
| Left | Previous keg page; pause automatic rotation for 20 seconds |
| Right | Next keg page; pause automatic rotation for 20 seconds |
| Middle, short press | Cycle brightness: 6%, 13%, 25% |
| Middle, hold 5 seconds | Open the protected Wi-Fi setup network; preserve current pairing and credentials |

The setup page is accessible only through the clock's AP at `192.168.4.1`, with a per-boot setup token. On a fully configured clock, setup mode closes automatically after 10 minutes. With missing Wi-Fi or scale settings it remains available. Saving settings restarts the clock; reopening setup does not automatically delete the pairing. The eight-character AP password is stored once and remains the same across restarts.

Brightness resets to 13% after restart. Battery sleep, battery telemetry, buzzer, and auto brightness are not implemented; this version is designed for continuous USB power.

## Build and flash on Windows

Use an **ESP-IDF 6.0.1 terminal**, matching the touchscreen and this project's CI toolchain. The TC001 target is classic `esp32`, not ESP32-S3/C3.

```powershell
cd C:\Users\kperry\Documents\GitHub\KegScaleESPDisplay
git fetch origin
git switch feature/tc001-pixel-display
git pull --ff-only
cd tc001
# Needed only if an older TC001 build folder/config still targets IDF 5.5.2:
idf.py fullclean
idf.py set-target esp32
idf.py build
```

Connect a USB-C **data cable**. Save the original 4 MiB flash before replacing it:

```powershell
# Replace COM5 with the actual port; stop any serial monitor first.
python -m esptool --chip esp32 --port COM5 read-flash 0x0 0x400000 tc001-original.bin
idf.py -p COM5 flash monitor
```

If it does not enter flashing mode automatically, follow the TC001/AWTRIX flashing instructions for the actual unit. Confirm successful backup before an erase is needed. `idf.py flash` writes this project's bootloader, partition table, and application; do not flash the e-paper/touchscreen binaries on the clock. Replacing stock firmware replaces its stock app functions.

The firmware does not silently erase NVS on startup errors. If stock/old NVS causes `ESP_ERR_NVS_NO_FREE_PAGES` or `ESP_ERR_NVS_NEW_VERSION_FOUND`, back up as needed and run `idf.py -p COM5 erase-flash`, then flash again. This erases saved Wi-Fi and pairings too. Old TC001 BLE pairing data is not used by this Wi-Fi version; remove its old BLE registration separately on the scale if you had paired the earlier firmware.

## Demo, hardware, and validation

`idf.py menuconfig` → **Keg TC001** → enable demonstration, then build/flash. It shows `DEMO` for the first 1.5 seconds of each page and does not start networking. Disable it and rebuild for live use.

Hardware mapping is grounded in AWTRIX's [TC001 docs](https://github.com/Blueforcer/awtrix3/blob/main/docs/hardware.md) and source: matrix GPIO32, 256 WS2812-compatible GRB pixels; left/middle/right buttons GPIO26/27/14, active low. Default wiring is row serpentine; menuconfig also supports four 8×8 row-progressive tiles and column serpentine. **Physical hardware is not yet tested.** Verify orientation/color order, button levels, setup AP, station reconnect, pairing/removal, and sustained encrypted state updates when the clock arrives.

USB firmware updates only for now. A dedicated TC001 OTA hardware/channel entry and matching scale-side support are needed before adding signed OTA; it does not install e-paper/touchscreen OTA images.

```bash
./tc001/tests/run.sh
python3 tc001/tools/build_preview.py  # Pillow required
```

Host tests cover pixel mapping, gauges/statuses, scrolling, allowed host strings, stale timeout, plaintext handshake gating, and bounded WebSocket frame assembly/generation checks. The shared crypto has its existing actual-source tests in `touchscreen/tests/controller_link_test.c`. CI builds live and demo firmware and runs host checks. Build/test success does not establish operation on physical hardware.
