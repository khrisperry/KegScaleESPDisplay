# Architecture

## Normal wake cycle

The display is designed around e-paper retention and deep sleep rather than a persistent BLE connection.

1. ESP32 wakes from the 180-second timer, the one-hour safety timer when frequent check-in is disabled, or native capacitive touch on GPIO12.
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

The logical ID is verified against the Scale's read-only device-info
characteristic before pairing is saved. Normal operation never selects a Scale
solely by RSSI.

For initial setup, pairing is web-driven:

- Start **Add e-paper display** from the Scale web interface.
- The Scale opens an explicit five-minute pairing window.
- The unpaired display discovers only pairing-enabled compatible Scales.
- The display generates a temporary six-digit code and shows the target
  `KegScale-XXXX` identity.
- Enter the code on the Scale web page to authorize authenticated LE Secure
  Connections bonding.
- The saved logical Scale identity/address is then used for exact reconnect.

## Capacitive touch wake

GPIO12 is ESP32 touch channel 5 and is configured as the native deep-sleep touch wake source. The touch controller self-calibrates against the untouched benchmark immediately before sleep and uses the scale-owned threshold received over an optional BLE characteristic. The value is configurable from the Scale web page and Home Assistant, persists
across deep sleep, and falls back to the firmware default of 1% when no valid
Scale-owned threshold is available. The normal 180-second timer can be disabled, but a 3,600-second safety timer remains armed so a touch-sensor problem cannot make the display unreachable.

The former GPIO39 EXT0 button wake is disabled because ESP32 touch wake and EXT0 wake cannot be enabled together.

## Display update policy

RTC memory remembers the last image-driving state across deep sleep without writing flash every three minutes.

The e-paper component also retains the last 4 KB rendered framebuffer in RTC
memory. A normal keg-screen update compares the new frame with that retained
copy and sends only the smallest byte-aligned rectangle containing changed
pixels. The retained copy is updated only after a successful panel refresh.
Setup, pairing, and status screens always receive a full refresh and invalidate
the keg-screen differential baseline. Every 50th changed keg-screen update is
forced to a full refresh to clear accumulated ghosting and reset the partial
update counter.

Firmware update offers are checked during scheduled maintenance wakes. The Scale
first authenticates the selected signed repository manifest and then exposes the
compatible offer to the bonded display. When an approved newer version is
offered, the display retrieves home Wi-Fi credentials and OTA metadata only over
the authenticated encrypted BLE session, downloads by HTTPS into the inactive
OTA slot, validates size and SHA-256, turns Wi-Fi off, and reboots. Normal wake
cycles never keep Wi-Fi enabled.

The display refreshes when:

- first valid state is obtained
- scale identity changes
- the scale's significant-change sequence changes
- profile revision changes
- whole servings remaining changes
- stable total weight differs by at least 0.5 lb
- the authenticated scale control requests a full refresh

An unstable/settling snapshot does not replace an already stable e-paper image.
A manual full-refresh command intentionally overrides this policy, redraws the
current keg screen, and resets the partial-update counter.

## Hardware assumptions and remaining characterization

The V2.3.1 board uses the published T5 pin map. Current LILYGO documentation
identifies DEPG0213BN as the default 2.13-inch panel option; GDEY0213B74 is
another supported panel. Both are SSD1680-class 122x250 panels and firmware uses
a 250x122 logical landscape framebuffer.

Panel initialization/orientation and normal full/partial-refresh behavior have
been exercised on the current hardware. Remaining physical characterization is
primarily actual deep-sleep current/awake-time measurement on the production
V2.3.1 hardware and revalidation if a different panel variant is introduced.


## Touch-pour observation window

Timer wakes remain fast one-shot checks.

A capacitive-touch wake is treated as a likely pour event instead:

1. Draw the touch acknowledgement.
2. Deep-sleep with only a timer for 10 seconds.
3. Read the paired Scale once.
4. If calibrated but unstable, and no force-refresh request is present, sleep another five seconds and read once more.
5. Refresh for a meaningful stable result, or retain the previous image if still settling. No third settling check is made.

The scale-side significant-change sequence is the primary signal. The display also retains defensive comparisons for servings, profile revision, stability, and >=0.5 lb total-weight changes.

This prevents the display from refreshing immediately when somebody first touches the tap, before the pour has actually changed and settled the scale.
