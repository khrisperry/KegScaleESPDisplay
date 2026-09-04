# Keg Scale BLE Protocol v1

This display consumes the read-only BLE service implemented by KegScaleESP.

## Discovery advertisement

The scale advertises an eight-byte manufacturer payload alongside its 128-bit service UUID. Bytes 0-1 are `4b 53`, byte 2 is advertisement version 1, and byte 3 contains flags. Bit 0 means display pairing is active. Bit 1 means bytes 4-7 contain the scale's IPv4 address octets.

While unpaired, the display continuously scans. If exactly one scale advertises a LAN address and no scale is currently pairing, the display renders `http://<address>/` as a QR code. This URL handoff is intentionally unauthenticated discovery data; all credentials, OTA bundles, and display-control data remain protected by the exact authenticated bond.

## UUIDs

| Item | UUID | Use |
| --- | --- | --- |
| Service | `8f7a0001-3f7b-4c61-a2b8-6d2f5b71c001` | Scan/discovery filter |
| Snapshot | `8f7a0002-3f7b-4c61-a2b8-6d2f5b71c001` | Read current state |
| Keg name | `8f7a0003-3f7b-4c61-a2b8-6d2f5b71c001` | Read display label |
| Device info | `8f7a0004-3f7b-4c61-a2b8-6d2f5b71c001` | Verify protocol and logical scale ID |
| Display configuration | `8f7a0005-3f7b-4c61-a2b8-6d2f5b71c001` | Serving size and layout metadata |
| Display update offer | `8f7a0006-3f7b-4c61-a2b8-6d2f5b71c001` | Latest compatible display firmware |
| Wi-Fi/OTA bundle | `8f7a0007-3f7b-4c61-a2b8-6d2f5b71c001` | Authenticated encrypted long read; exact bonded display only |
| Display control | `8f7a0008-3f7b-4c61-a2b8-6d2f5b71c001` | Authenticated encrypted read; remove/unpair command |
| Touch configuration | `8f7a000a-3f7b-4c61-a2b8-6d2f5b71c001` | Optional runtime touch threshold |

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

The display consumes the scale-owned appearance configuration:

- layout 1: servings/pints focused
- layout 2: percent-full focused
- layout 3: diagnostics
- flag bit 0: show beer name
- flag bit 1: show gallons remaining
- flag bit 2: show percent full
- flag bit 3: show serving size
- flag bit 4: show total keg weight
- flag bit 5: place the beer name at the top in layouts 1 and 2
- flag bit 7: configuration is explicitly present

If bit 7 is absent, the display treats the payload as a legacy configuration
and enables the original full metric set. A changed configuration revision
forces an e-paper refresh even if the keg reading is otherwise unchanged.

## Firmware compatibility

The display OTA manifest declares its protocol version and supported scale
protocol range. The scale only exposes the authenticated OTA offer after the
manifest is compatible and the exact version has been approved (or display
auto-update is enabled).

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

### Touch configuration

The optional touch-configuration characteristic keeps the existing display
configuration packet unchanged for backward compatibility:

`8f7a000a-3f7b-4c61-a2b8-6d2f5b71c001`

| Offset | Size | Field |
| ---: | ---: | --- |
| 0 | 1 | protocol version |
| 1 | 1 | threshold percentage below the untouched baseline |
| 2 | 1 | display/config revision |

Valid thresholds are 1–50%. Lower values are more sensitive. If an older scale
does not expose this characteristic, or a value is invalid, the display uses its
8% firmware default.

## Encrypted display OTA

The update offer is non-secret and may be read during the normal wake cycle. If its hardware ID matches `lilygo_t5_v2_3_1` and the version differs from the running application, the display reconnects using its persistent BLE bond and establishes an authenticated encrypted session. No pairing code is stored or reused.

The Wi-Fi/OTA bundle requires authenticated encryption and the scale additionally verifies that the connected peer is the specifically authorized bonded display. It includes the SSID, password, HTTPS URL, image size, version, hardware ID, and SHA-256 digest. Credentials remain in RAM only and are cleared after Wi-Fi is stopped.

The display-control characteristic is also restricted to the exact authorized bond. A remove/replace request sets an unpair flag; after reading it, the display clears its local bond and pairing identity.
