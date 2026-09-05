# KegScaleESPDisplay

ESP-IDF firmware for the LILYGO T5 V2.3.1 2.13-inch e-paper companion display for KegScaleESP.

The display is a low-power BLE client. It wakes, connects to one paired Keg Scale, reads the current scale snapshot, refreshes e-paper only when useful display data changed, disconnects, and returns to deep sleep.

Initial hardware target:

- LILYGO T5 V2.3.1_2.13
- ESP32
- 4 MB flash
- 2.13-inch monochrome e-paper (122x250 native / 250x122 landscape, SSD1680-class panel)
- Timer wake every 180 seconds
- Native capacitive touch wake on GPIO12 (ESP32 touch channel 5)
- Timer wake every 180 seconds

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

## Development workflow

Changes are developed on feature branches, compiled in GitHub Actions, hardware-tested, and merged after validation.


## Current bring-up behavior

The initial firmware is already structured around the final low-power workflow:

- timer wake every 3 minutes
- native GPIO12 capacitive-touch wake
- direct reconnect to one saved scale
- exact-ID recovery scan if the BLE address changes
- one-shot reads of status, keg name, and device information
- e-paper refresh only for meaningful changes
- deep sleep after each successful check
- no persistent Wi-Fi connection

The display keeps the last image visible while sleeping.

When GPIO12 wakes the display, a small check-mark badge appears in the top-left corner using the panel's fast partial-refresh path before the pour wait or BLE check begins. A normal full refresh clears it; if the scale data does not require a full refresh, the firmware removes the badge with a second partial update before returning to sleep.

The partial-refresh driver detects whether the V2.3.1 board carries the GDEM0213B74 panel or LILYGO's default DEPG0213BN panel and uses the matching waveform and activation sequence.

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

## Hardware validation order

1. Confirm firmware boots and scans BLE.
2. Confirm it discovers the existing scale and reads protocol version 1.
3. Confirm one-scale automatic pairing or explicit serial pairing.
4. Confirm current keg values decode correctly.
5. Confirm the e-paper panel initializes and orientation is correct.
6. Confirm a meaningful scale change updates e-paper.
7. Confirm a no-change 3-minute wake does not refresh e-paper.
8. Confirm touching the GPIO12 electrode wakes from deep sleep and logs `wake=touch`.
9. Tune the touch threshold percentage if needed.
10. Measure actual V2.3.1 deep-sleep battery current.


## Capacitive touch wake

The current hardware configuration uses the ESP32's native capacitive touch input on **GPIO12 / touch channel 5**. Before each deep sleep the firmware measures the untouched baseline and applies the scale-owned threshold. It defaults to 8% below baseline and can be changed from the scale web page or Home Assistant; lower values are more sensitive.

The timer wake remains enabled at the same time. The previous GPIO39 EXT0 button wake has been removed because the classic ESP32 cannot use EXT0 and touch wake simultaneously.

For a direct touch electrode, connect the electrode to GPIO12. A 470 ohm to 2 kohm series resistor near the ESP32 is recommended for noise/ESD protection; 510 ohms is a good starting value.

If an active 3-pin capacitive-touch module is used instead of a passive electrode, its digital output should be treated as a GPIO wake signal rather than the ESP32 native touch input and the firmware configuration should be changed accordingly.


### Touch wake waits for the pour

A timer wake performs the normal quick BLE check. A GPIO12 capacitive-touch wake assumes a pour may be starting, so it waits 10 seconds before the first scale read and then retries every 2 seconds until a new stable meaningful scale state is available or 20 seconds total have elapsed. The e-paper keeps showing the previous valid state during this observation window and is refreshed only once at the end of a real pour.
The scale also coordinates display firmware updates. The display reads a version offer over BLE, retrieves home Wi-Fi credentials and update metadata only through an authenticated encrypted BLE session, performs an HTTPS A/B OTA download, validates the image size and SHA-256 digest, and turns Wi-Fi back off before rebooting.


When periodic check-in is disabled, the display clears every stale wake source before deep sleep and then arms only GPIO12 capacitive touch.
