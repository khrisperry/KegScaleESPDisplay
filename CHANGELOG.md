# Changelog

## V1.3.4 - 2026-09-23

Guided pairing/cancel improvements, e-paper touch wizard removal and 1% default,
OTA/crypto hardening, and connection/wake regression suites. Scale web Dashboard
and Glass views now match serving-size vessels and fit phones. See
[complete release notes](docs/releases/V1.3.4.md).

## V1.0.0 - 2026-09-06

First production release of the KegScaleESP companion display.

### Setup and pairing

- Shows the scale's LAN address as a QR code during first-run setup.
- Pairs from the scale webpage with a display-generated six-digit code and an
  authenticated BLE bond.
- Reconnects only to the saved logical scale identity and repairs a changed BLE
  address after identity validation.

### Display

- Uses bundled native-resolution Keg Display Sans fonts on setup, pairing, keg,
  and diagnostic screens.
- Shows battery state in six levels: 0%, 20%, 40%, 60%, 80%, and 100%.
- Acknowledges capacitive touch immediately with a top-left partial refresh.
- Initializes both e-paper RAM planes on full refresh to prevent stray edge dots.
- Uses a hardware-validated DEPG0213BN partial-update waveform and settling
  timing for the LILYGO T5 V2.3.1 2.13-inch display.

### Power and updates

- Supports configurable GPIO12 capacitive-touch sensitivity with a 3% default.
- Supports optional periodic check-in or strict touch-only deep sleep.
- Waits for a likely pour to settle before replacing the retained e-paper image.
- Installs compatible display firmware through authenticated BLE metadata and
  validated HTTPS A/B OTA.
