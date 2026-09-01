# Architecture

## Normal wake cycle

The display is designed around e-paper retention and deep sleep rather than a persistent BLE connection.

1. ESP32 wakes from the 180-second timer or native capacitive touch on GPIO12.
2. Load the one paired scale identity from NVS.
3. Connect directly to that saved BLE address.
4. Discover/read the Keg Scale protocol.
5. If the direct address fails, scan only for the exact saved logical `KegScale-XXXX` ID and repair its address after protocol validation.
6. Compare the scale's significant-change sequence and display values with RTC-retained state.
7. Refresh e-paper only when useful data changed.
8. Disconnect BLE and enter deep sleep.

The current e-paper image remains visible while the ESP32 sleeps.

## Correct-scale selection

The display never chooses a scale by RSSI during normal operation.

A pairing record contains:

- logical scale ID such as `KegScale-B560`
- BLE address
- BLE address type

The logical ID is verified against the scale's read-only device-info characteristic before pairing is saved.

For initial setup:

- exactly one compatible scale: optional automatic selection
- more than one compatible scale: no automatic choice
- current bring-up selection: serial `pair KegScale-XXXX`
- future selection: touch UI or temporary setup portal calling the same pairing API

## Capacitive touch wake

GPIO12 is ESP32 touch channel 5 and is configured as the native deep-sleep touch wake source. The touch controller self-calibrates against the untouched benchmark immediately before sleep and uses a configurable threshold percentage. The normal 180-second timer wake remains enabled in parallel.

The former GPIO39 EXT0 button wake is disabled because ESP32 touch wake and EXT0 wake cannot be enabled together.

## Display update policy

RTC memory remembers the last image-driving state across deep sleep without writing flash every three minutes.

The display refreshes when:

- first valid state is obtained
- scale identity changes
- the scale's significant-change sequence changes
- profile revision changes
- whole servings remaining changes
- stable total weight differs by at least 0.5 lb

An unstable/settling snapshot does not replace an already stable e-paper image.

## Hardware assumptions needing physical validation

The V2.3.1 board uses the published T5 pin map. Current LILYGO documentation identifies DEPG0213BN as the default 2.13-inch panel option; GDEY0213B74 is another supported panel. Both are SSD1680-class 122x250 panels and firmware currently uses a 250x122 logical landscape framebuffer.

Physical testing must confirm:

- panel orientation
- busy polarity/timing
- full-refresh waveform behavior
- whether the exact installed panel needs a different init profile
- actual deep-sleep current on this V2.3.1 board revision
