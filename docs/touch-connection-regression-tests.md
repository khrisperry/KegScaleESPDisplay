# Touch pairing and reconnect regression tests

Validated locally September 22, 2026. Run `bash tests/run_host_tests.sh` from
the Display repository in Linux/WSL, or `bash touchscreen/tests/run_host_tests.sh`
for the Touch suite alone. Host dependencies are Python 3, C/C++ compilers,
`libmbedtls-dev`, and `libcjson-dev`. A configured Touch project's managed cJSON
source is used when available; otherwise the runner links the system cJSON library.
The Touch CI test job installs both development libraries.

`run_connection_test.py` extracts complete production functions and type definitions
from `main.cpp`, `main_wrapper.cpp`, and `app.h`. It compiles them with transport,
clock, persistence, cryptography, and UI fakes, using real cJSON parsing. No copy
of the connection algorithm is maintained in the harness.

Covered behavior:

- Disconnect during pairing, reconnect handshake, authorization, and failed save.
- Remove/reset the local session and reconnect immediately; old authorization
  frames cannot restore the removed pairing.
- Saved pairings bypass setup HTTP probing; inactive unpaired slots do not probe.
- Selecting another Scale slot preserves independent connection state and keeps
  background disconnects from replacing the selected Scale's UI.
- Old WebSocket callbacks and already-queued frames are rejected by generation.
- Remote cancellation and disconnect during local cancellation stop retries.
- Initialization/start failures and Wi-Fi loss retain the appropriate retry policy.
- Split WebSocket messages assemble; oversized and out-of-order fragments reject.
- Missing acknowledgements report an unknown outcome and do not replay commands
  after reconnect. Mismatched, late, plaintext, and unauthenticated results cannot
  complete a pending command.
- OTA is deferred during Wi-Fi loss, saved-session reconnect, an active pairing
  socket, or a pending command, and allowed after authenticated state resumes.

The full Display/Touch host suite and ESP-IDF 6.0.1 Touch build passed. No firmware
behavior changes were needed for these cases, and nothing was flashed or published.

## Limits and hardware checks

Transport, cryptography, persistence, and UI calls are faked in this harness.
Separate existing protocol tests exercise real cryptography, and overlay tests
exercise the countdown/cancel UI logic. These tests do not prove radio timing,
real scheduler interleavings, actual NVS persistence, or LVGL object lifetime.
The removal scenario tests the connection transition, not the HTTP cleanup guard.
Discovery/scan callbacks after screen destruction remain separate backlog work.

On hardware, verify pairing interruption and retry, remove/immediate re-pair,
switching two saved Scales, Wi-Fi loss during a save, and OTA attempts during
reconnect. Confirm a lost acknowledgement shows an unknown outcome; check the
Scale's actual saved value before manually retrying. Confirm both display types
remain usable while pairing the other.
