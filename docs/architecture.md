# Architecture

## Normal wake cycle

The display is designed around e-paper retention and deep sleep rather than a persistent BLE connection.

1. ESP32 wakes from the optional 180-second timer or native capacitive touch on GPIO12.
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

For initial setup, an unpaired display shows the LAN address advertised by one
compatible scale as a QR code. The scale webpage wizard opens an explicit pairing
window, the display presents a random six-digit code, and the user enters that
code in the wizard. If multiple scales are pairing, the display does not guess.

## Capacitive touch wake

GPIO12 is ESP32 touch channel 5 and is configured as the native deep-sleep touch wake source. The touch controller self-calibrates against the untouched benchmark immediately before sleep and uses the scale-owned threshold received over an optional BLE characteristic. The value is configurable from the scale web page and Home Assistant, persists across deep sleep, and falls back to 3% with older scale firmware. The normal 180-second timer wake is optional; touch-only mode explicitly clears it before sleeping.

The former GPIO39 EXT0 button wake is disabled because ESP32 touch wake and EXT0 wake cannot be enabled together.

## Display update policy

RTC memory remembers the last image-driving state across deep sleep without writing flash every three minutes.

Firmware update offers are checked during a normal BLE read. When a compatible new version is offered, the display uses its authenticated bond to retrieve the home Wi-Fi and OTA metadata in RAM, downloads by HTTPS into the inactive OTA slot, validates the manifest size and SHA-256 digest, turns Wi-Fi off, and reboots. Normal wake cycles never start Wi-Fi.

The display refreshes when:

- first valid state is obtained
- scale identity changes
- the scale's significant-change sequence changes
- profile revision changes
- whole servings remaining changes
- stable total weight differs by at least 0.5 lb

An unstable/settling snapshot does not replace an already stable e-paper image.

## E-paper implementation

The V2.3.1 board uses the published T5 pin map. Current LILYGO documentation identifies DEPG0213BN as the default 2.13-inch panel option; GDEY0213B74 is another supported panel. Both are SSD1680-class 122x250 panels and firmware currently uses a 250x122 logical landscape framebuffer.

Full refresh initializes both SSD1680 RAM planes and the update-control register.
The immediate touch acknowledgement uses the DEPG0213BN partial-update waveform,
BUSY monitoring, and minimum power-on/update settling intervals validated on the
target display. The top-left acknowledgement area is reserved so a full refresh
always restores a clean baseline.


## Touch-pour observation window

Timer wakes remain fast one-shot checks when periodic check-in is enabled.

A capacitive-touch wake is treated as a likely pour event instead:

1. Keep the existing e-paper image unchanged.
2. Wait 10 seconds before the first BLE read.
3. Read the paired scale.
4. If the result is unchanged or settling, wait 2 seconds and read again.
5. As soon as a meaningful stable state is observed, refresh e-paper once and sleep.
6. If no meaningful stable change arrives by 30 seconds total, keep the existing image and return to sleep.

The scale-side significant-change sequence is the primary signal. The display also retains defensive comparisons for servings, profile revision, stability, and >=0.5 lb total-weight changes.

In touch-only mode, the display instead performs one BLE read after the initial
10-second delay and returns to deep sleep with only capacitive touch armed.
