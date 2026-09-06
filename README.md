# KegScaleESPDisplay

ESP-IDF firmware for the LILYGO T5 V2.3.1 2.13-inch e-paper companion display for KegScaleESP.

The display is a low-power BLE client. It wakes, connects to one paired Keg Scale, reads the current scale snapshot, refreshes e-paper only when useful display data changed, disconnects, and returns to deep sleep.

Supported hardware:

- LILYGO T5 V2.3.1_2.13
- ESP32
- 4 MB flash
- 2.13-inch monochrome e-paper (122x250 native / 250x122 landscape, SSD1680-class panel)
- Native capacitive touch wake on GPIO12 (ESP32 touch channel 5)
- Optional periodic wake every 180 seconds

The scale remains the source of truth. This display does not perform tare, calibration, keg-profile editing, Wi-Fi configuration, Home Assistant, or scale OTA.

## Pairing model

The display stores exactly one scale identity in NVS and NimBLE stores the authenticated BLE bond keys in NVS. The six-digit pairing code is temporary and is never stored.

Initial setup is driven entirely from the scale web interface:

1. After Wi-Fi provisioning, leave the unpaired display powered on near the scale. It reads the scale's advertised LAN address and shows a QR code that opens the first-run setup wizard.
2. Complete calibration and the keg profile, then start the wizard's **Display** step.
3. Click **Start display pairing**. The scale advertises an explicit five-minute pairing window.
4. The display generates a random six-digit code and shows the target KegScale-XXXX identity plus the code on e-paper.
5. Enter the display code in the wizard and click **Pair display**.
6. NimBLE creates an authenticated LE Secure Connections bond on both devices.
7. The display saves only the scale identity/address, renders the current keg state, and returns to its normal sleep cycle.

On every normal wake the display reconnects only to its saved scale identity. It never chooses a scale by signal strength.

**Remove display** is delivered as an authenticated command on the display's next wake. **Replace display** first removes the old bond and then opens a new pairing window. If the original scale is permanently unavailable, three deliberate power cycles before the display reaches its normal deep-sleep path clear the saved pairing and return the display to recovery pairing mode. USB/NVS erase remains the last-resort service recovery method.

## Source scale BLE protocol

Service UUID:

`8f7a0001-3f7b-4c61-a2b8-6d2f5b71c001`

Characteristics:

- `...0002...` - 20-byte status snapshot, Read + Notify
- `...0003...` - keg name, Read + Notify
- `...0004...` - device/protocol/target/firmware information, Read

Protocol version 1 is read-only.

## Hardware pins

LILYGO's published V2.3.1 pin map:

| Function | GPIO |
| --- | ---: |
| E-paper MOSI | 23 |
| E-paper SCLK | 18 |
| E-paper DC | 17 |
| E-paper BUSY | 4 |
| E-paper RST | 16 |
| E-paper CS | 5 |
| Battery ADC | 35 |
| BOOT button | 0 |
| Button 1 | 39 |

V2.3.1 does **not** have the GPIO12 display-power switch added in V2.4.

## Release channels

Display firmware is built and published by GitHub Actions. `main` publishes the
production OTA channel, `beta` publishes beta, and `dev` publishes development
builds. Every release uses the semantic version in `version.txt`.

## Production behavior

Version 1.0.0 provides the complete low-power companion-display workflow:

- optional periodic wake every 3 minutes
- native GPIO12 capacitive-touch wake
- direct reconnect to one saved scale
- exact-ID recovery scan if the BLE address changes
- one-shot reads of status, keg name, and device information
- e-paper refresh only for meaningful changes
- deep sleep after each successful check
- no persistent Wi-Fi connection
- six-level battery indicator in the top-right corner
- immediate touch acknowledgement in the top-left corner

The display keeps the last image visible while sleeping.

When GPIO12 wakes the display, a small check-mark badge appears in the top-left corner using the panel's fast partial-refresh path before the pour wait or BLE check begins. A normal full refresh clears it; if the scale data does not require a full refresh, the firmware removes the badge with a second partial update before returning to sleep.

The partial-refresh path uses LILYGO's default DEPG0213BN waveform and minimum settling intervals so an unreliable BUSY transition cannot cut the update short.

All setup, pairing, connection-status, QR, keg, and diagnostics screens use the bundled Keg Display Sans font. Full e-paper refreshes initialize both SSD1680 RAM planes and the display-update control register, preventing random controller RAM from appearing as a dotted line along the panel edge after a cold boot.

### First pairing

No serial interaction is required for normal pairing. Start **Add display** from the scale web page and enter the one-time six-digit code shown on the e-paper display.

An unpaired display stays awake and scans for an explicitly pairing-enabled scale, so initial setup does not wait for the normal three-minute sleep interval.

## Build

ESP-IDF 6.0.1:

```bash
idf.py set-target esp32
idf.py build
idf.py -p COMx flash monitor
```

The GitHub Actions build also targets classic ESP32.

## Release validation

Before promoting a release, confirm:

1. The ESP-IDF build and OTA publishing workflows succeed.
2. Initial setup shows the scale webpage QR code and completes authenticated pairing.
3. Keg values, the selected layout, fonts, and battery icon render correctly.
4. A meaningful stable scale change refreshes the display and an unchanged reading does not.
5. Touch wake logs `wake=touch` and immediately shows the top-left acknowledgement.
6. Touch-only mode returns to deep sleep with no timer wake armed.
7. A display OTA update downloads, validates, boots, and is confirmed successfully.


## Capacitive touch wake

The current hardware configuration uses the ESP32's native capacitive touch input on **GPIO12 / touch channel 5**. Before each deep sleep the firmware measures the untouched baseline and applies the scale-owned threshold. It defaults to 3% below baseline and can be changed from the scale web page or Home Assistant; lower values are more sensitive.

Periodic check-in can be enabled alongside touch wake or disabled for strict touch-only operation. Before deep sleep, firmware clears all previously armed wake sources and enables only the selected timer and/or touch source. The previous GPIO39 EXT0 button wake has been removed because the classic ESP32 cannot use EXT0 and touch wake simultaneously.

For a direct touch electrode, connect the electrode to GPIO12. A 470 ohm to 2 kohm series resistor near the ESP32 is recommended for noise/ESD protection; 510 ohms is a good starting value.

If an active 3-pin capacitive-touch module is used instead of a passive electrode, its digital output should be treated as a GPIO wake signal rather than the ESP32 native touch input and the firmware configuration should be changed accordingly.


### Touch wake waits for the pour

A timer wake performs the normal quick BLE check. When periodic check-in is enabled, a GPIO12 capacitive-touch wake assumes a pour may be starting: it waits 10 seconds before the first scale read and then retries every 2 seconds until a new stable meaningful scale state is available or 30 seconds total have elapsed. In touch-only mode, it performs one scale read after the 10-second delay and returns to sleep. The e-paper keeps showing the previous valid keg state during the observation window and is refreshed only once when the scale reports a real change.
The scale also coordinates display firmware updates. The display reads a version offer over BLE, retrieves home Wi-Fi credentials and update metadata only through an authenticated encrypted BLE session, performs an HTTPS A/B OTA download, validates the image size and SHA-256 digest, and turns Wi-Fi back off before rebooting.


When periodic check-in is disabled, the display clears every stale wake source before deep sleep and then arms only GPIO12 capacitive touch.
