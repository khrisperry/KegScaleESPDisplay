# Wi-Fi touchscreen controller - V1.3.4

This is a separate ESP-IDF application for the **Waveshare ESP32-S3-Touch-LCD-4B** (480 × 480, 16 MB flash, 8 MB octal PSRAM). It uses Wi-Fi only and remains awake on external power. The repository root continues to build the existing ESP32 BLE e-paper display. Do not flash the root project's image onto this board or use this image on the e-paper display.

## First installation

1. Update the scale to **V0.0.31 or later with Wi-Fi controller protocol 1**. The scale's existing BLE e-paper pairing stays intact.
2. In an ESP-IDF **v6.0.1** terminal, open this subdirectory, not the repository root:

   ```powershell
   cd C:\Users\kperry\Documents\GitHub\KegScaleESPDisplay
   git pull origin dev
   cd touchscreen
   idf.py set-target esp32s3
   idf.py build
   idf.py -p COM13 flash monitor
   ```

   Replace COM13 with this board's actual port. The first installation requires USB flashing because the new application has its own partition layout. Press Ctrl+] to exit the monitor.
3. On **Setup**, scan/select Wi-Fi (or enter its SSID), enter the password, then Save and connect. The device applies saved connection settings in place.
4. On the scale's web page, select **Wi-Fi touchscreen setup → Add touchscreen**. Pairing stays open for five minutes.
5. On the touchscreen's Setup page, select **Find scale**, or enter its IPv4 address/hostname. Save and connect. Discovery needs the same LAN/VLAN; routed connections require network access to the scale on TCP port 80. Discovery never silently selects among several scales.
6. The touchscreen displays a six-character hexadecimal pairing code. Enter that exact code on the scale's setup page and authorize it. Successful pairing opens the dashboard.

If confirmation is interrupted during first pairing, remove the touchscreen pairing on the scale and reopen Add touchscreen. The existing e-paper pairing is separate.

## Screens

- **Home:** Dashboard and Glass views with beverage name, servings, fill gauge, gallons, serving size, weight and connection/stability status. Glass vessels follow 12/16/20/32/64 oz serving sizes. Disconnected readings are explicitly marked with their age.
- **Keg:** Name, capacity, empty keg weight, beverage density and serving size. Save is confirmed only after the scale saves it. Stale edits are rejected; use Reload from scale to reconcile changes from the web page or Home Assistant.
- **Replace keg:** Leads to the keg form with replacement instructions. Do not tare with a keg on the scale.
- **Scale:** Start calibration, remove all objects, save empty tare, apply a known weight, then calibrate. Each step advances only after the scale confirms it. A two-minute lease excludes other calibration callers and expires automatically; Cancel releases it.
- **Setup:** Wi-Fi, scale discovery/address, brightness and pairing removal. The on-screen keyboard appears when an input is selected. Scroll the form to reach its remaining fields and buttons.
- **Update:** Firmware versions and explicit installation of development touchscreen firmware. Development updates require confirmation and should only be used for instructed testing.

The scale remains authoritative. The touchscreen does not calculate its own independent keg totals. The e-paper display receives changes at its next BLE check-in.

## Updates and compatibility

The manually dispatched `Wi-Fi Touchscreen` workflow builds this application.
Validated firmware is published under `KegScaleFirmware/touchscreen/<channel>/esp32s3/`
for production, beta, or dev. This feed is separate from Scale and e-paper firmware.
HTTPS, hardware/target, image SHA-256, application identity, and version are checked.

Two Scale profiles can be saved and selected. Each Scale supports one authorized
Wi-Fi touchscreen, independently of its BLE e-paper display. Saved sessions
reconnect automatically; commands with lost acknowledgements are not replayed.
OTA waits while the active saved connection recovers or a command is pending.

Pairing has a countdown and Cancel button, and hides home-view controls. You can
cancel from the Scale webpage or the Touch Display and start again.

## Protocol and validation

See the scale repository's `docs/wifi-controller.md` for the shared protocol. The `controller_link` component is mirrored exactly in both repositories. Changes to its wire format require coordinated versioning.

Host protocol tests execute the actual C encryption component against PSA Crypto:

```bash
sudo apt-get install libmbedtls-dev libcjson-dev
bash touchscreen/tests/run_host_tests.sh
```

Hardware acceptance: verify touch alignment and all scrollable forms; pair, reboot both devices, edit from the touchscreen and web page, force a conflicting edit, unplug the access point, run tare/calibration, install touchscreen OTA, and verify the original e-paper check-in still works. Build and host tests cannot establish physical touch accuracy or RF reliability.
