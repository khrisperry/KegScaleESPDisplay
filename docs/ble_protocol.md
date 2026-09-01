# Keg Scale BLE Protocol v1

This display consumes the read-only BLE service implemented by KegScaleESP.

## UUIDs

| Item | UUID | Use |
| --- | --- | --- |
| Service | `8f7a0001-3f7b-4c61-a2b8-6d2f5b71c001` | Scan/discovery filter |
| Snapshot | `8f7a0002-3f7b-4c61-a2b8-6d2f5b71c001` | Read current state |
| Keg name | `8f7a0003-3f7b-4c61-a2b8-6d2f5b71c001` | Read display label |
| Device info | `8f7a0004-3f7b-4c61-a2b8-6d2f5b71c001` | Verify protocol and logical scale ID |

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
