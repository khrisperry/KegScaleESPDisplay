# Keg Scale BLE Protocol v1

This display consumes the read-only BLE service implemented by KegScaleESP.

## UUIDs

| Item | UUID | Use |
| --- | --- | --- |
| Service | `8f7a0001-3f7b-4c61-a2b8-6d2f5b71c001` | Scan/discovery filter |
| Snapshot | `8f7a0002-3f7b-4c61-a2b8-6d2f5b71c001` | Read current state |
| Keg name | `8f7a0003-3f7b-4c61-a2b8-6d2f5b71c001` | Read display label |
| Device info | `8f7a0004-3f7b-4c61-a2b8-6d2f5b71c001` | Verify protocol and logical scale ID |
| Display configuration | `8f7a0005-3f7b-4c61-a2b8-6d2f5b71c001` | Serving size and layout metadata |
| Display update offer | `8f7a0006-3f7b-4c61-a2b8-6d2f5b71c001` | Latest compatible display firmware |
| Wi-Fi/OTA bundle | `8f7a0007-3f7b-4c61-a2b8-6d2f5b71c001` | Authenticated encrypted long read |

## 20-byte snapshot

All multi-byte values are little-endian.

| Offset | Size | Field | Display conversion |
| ---: | ---: | --- | --- |
| 0 | 1 | protocol version | must equal 1 |
| 1 | 1 | flags | status bits |
| 2 | 2 | significant-change sequence | compare with RTC-retained sequence |
| 4 | 2 | remaining percent x100 | divide by 100 |
| 6 | 2 | whole servings remaining | direct |
| 8 | 2 | remaining gallons x100 | divide by 100 |
| 10 | 4 | total weight, milli-lb | divide by 1000 |
| 14 | 4 | beverage weight, milli-lb | divide by 1000 |
| 18 | 1 | scale Wi-Fi RSSI | diagnostic only |
| 19 | 1 | profile revision | detect keg-profile changes |

Flags:

- bit 0: calibrated
- bit 1: stable
- bit 2: profile configured
- bit 3: keg ready
- bit 4: scale Wi-Fi connected
- bit 5: tare set

## Sleeping-client behavior

The display does not depend on BLE notifications because deep sleep disconnects the client.

On wake it reads the current snapshot. The scale-side sequence counter advances only for meaningful stable events, so a sleeping client can discover that something changed even if it missed the original notification.

The display still compares actual fields defensively and never replaces a stable e-paper image with a settling snapshot.


## Display configuration

Optional characteristic:

`8f7a0005-3f7b-4c61-a2b8-6d2f5b71c001`

The display reads this 6-byte packet when available:

| Offset | Size | Field |
| ---: | ---: | --- |
| 0 | 1 | protocol version |
| 1 | 1 | layout ID |
| 2 | 1 | display/config revision |
| 3 | 1 | future flags |
| 4 | 2 | serving size in ounces x100 |

Serving size lets the display show both the configured size and a friendly remaining-unit label, e.g. 12 oz -> CANS LEFT and 16 oz -> PINTS LEFT.

For compatibility with older scale firmware that does not yet expose this characteristic, the display falls back to 16 oz / serving-focused layout.

## Encrypted display OTA

The update offer is non-secret and may be read during the normal wake cycle. If its hardware ID matches `lilygo_t5_v2_3_1` and the version differs from the running application, the display reconnects using its saved six-digit scale PIN and establishes a bonded LE Secure Connections session.

The Wi-Fi/OTA bundle requires authenticated encryption. It includes the SSID, password, HTTPS URL, image size, version, hardware ID, and SHA-256 digest. Credentials remain in RAM only and are cleared after Wi-Fi is stopped.
