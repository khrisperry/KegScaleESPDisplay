# KegScaleESPDisplay

ESP-IDF firmware for the LILYGO T5 V2.3.1 2.13-inch e-paper companion display for KegScaleESP.

The display is a low-power BLE client. It wakes, connects to one paired Keg Scale, reads the current scale snapshot, refreshes e-paper only when useful display data changed, disconnects, and returns to deep sleep.

Initial hardware target:

- LILYGO T5 V2.3.1_2.13
- ESP32
- 4 MB flash
- 2.13-inch monochrome e-paper (212x104, SSD1680-class panel)
- Timer wake every 180 seconds
- Button 1 (GPIO39) as a temporary manual wake source
- Capacitive touch wake will be added later

The scale remains the source of truth. This display does not perform tare, calibration, keg-profile editing, Wi-Fi configuration, Home Assistant, or scale OTA.

## Pairing model

The display stores exactly one scale identity in NVS.

On every normal wake it scans only for the Keg Scale BLE service and connects only to the saved scale identity. It does not connect to whichever scale happens to have the strongest signal.

During initial setup:

1. Scan for Keg Scale peripherals.
2. If exactly one compatible scale is found, the display may select and save it automatically.
3. If multiple compatible scales are found, the display refuses to guess.
4. Until a touch/setup UI is added, the serial console can list candidates and select one by its advertised `KegScale-XXXX` identity.
5. Pairing stores the BLE address, address type, and logical advertised scale ID.

Later captive-portal or touch configuration can reuse the same pairing component without changing BLE connection behavior.

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
