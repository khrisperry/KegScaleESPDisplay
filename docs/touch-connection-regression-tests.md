# Touch pairing and reconnect regression tests

Validated locally September 23, 2026. Run `bash tests/run_host_tests.sh` from
the Display repository in Linux/WSL, or `bash touchscreen/tests/run_host_tests.sh`
for the Touch suite alone. Host dependencies are Python 3, C/C++ compilers,
`libmbedtls-dev`, and `libcjson-dev`. A configured Touch project's managed cJSON
source is used when available; otherwise the runner links the system cJSON library.
The Touch CI test job installs both development libraries.

`run_connection_test.py` extracts complete production functions and type definitions
from `main.cpp` and `app.h`. V1.3.6 Dev removes `main_wrapper.cpp`; the
runtime settings, OTA, discovery, and synchronized-pairing paths now use explicit
calls in the production source. The harness compiles them with transport,
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
- Setup UI actions carry a content-generation token. Wi-Fi scan and mDNS discovery
  results/messages are rejected if the Setup screen was rebuilt or destroyed
  before the asynchronous work completes, including leave-and-return flows.

The full Display/Touch host suite passed under WSL on September 23, 2026,
including the UI lifetime generation guard, connection regression suite, controller-link
cryptography tests, and injected crypto-failure coverage. The ESP-IDF 6.0.1 Touch build
also passed, and Touch V1.3.5 was flashed and exercised on hardware. Repeatedly leaving
and returning to Setup while scale discovery was running did not crash, corrupt the UI,
or apply stale discovery results to the rebuilt screen. Production main remains V1.3.4
until this dev increment is promoted.

## Limits and hardware checks

Transport, cryptography, persistence, and UI calls are faked in this harness.
Separate existing protocol tests exercise real cryptography, and overlay tests
exercise the countdown/cancel UI logic. These tests do not prove radio timing,
real scheduler interleavings or actual NVS persistence. The UI lifetime regression
tests the production generation guard and its callback wiring, but host tests still
do not prove LVGL's internal object allocator behavior on hardware. The removal
scenario tests the connection transition, not the HTTP cleanup guard.

On hardware, verify pairing interruption and retry, remove/immediate re-pair,
switching two saved Scales, Wi-Fi loss during a save, and OTA attempts during
reconnect. Confirm a lost acknowledgement shows an unknown outcome; check the
Scale's actual saved value before manually retrying. Confirm both display types
remain usable while pairing the other.


## V1.3.6 main-wrapper removal checkpoint

Touch V1.3.6 Dev removes the source-inclusion/macro-interception
`main_wrapper.cpp`. The application now builds `main.cpp` directly. Live
settings apply, OTA scheduling, automatic discovery, local pairing cleanup, and
authenticated scale-requested unpair handling are explicit production calls.
`main_architecture_guard_test.py` fails if the wrapper returns, CMake stops
building `main.cpp` directly, or the key explicit hooks disappear.

Host and ESP-IDF build validation are required before hardware acceptance. On
hardware, verify saved reconnect, Remove pairing synchronization, fresh
six-character pairing, Save & connect without reboot, auto-discovery, and manual
Touch OTA/reconnect behavior.


## V1.3.6 pairing-rearm isolation fix — hardware validation pending

September 24 field-style hardware testing found that the pairing cleanup guard
re-armed an Add touchscreen window by disconnecting the touchscreen's entire
Wi-Fi station. With two configured Scales, removing/re-pairing Scale 2 therefore
dropped Scale 1's otherwise healthy WebSocket too.

Dev now queues a slot-specific `pairing_rearm` action to the main task instead.
The pairing guard never disconnects/reconnects station Wi-Fi. If the repaired
Scale is inactive, the active Scale is left untouched and the user is prompted
to select the repaired slot in Setup; if it is active, only that Scale's
connection is restarted. `pairing_rearm_isolation_guard_test.py` prevents the
global Wi-Fi reset path from returning.

Hardware acceptance: keep Scale 1 online, remove Scale 2, open Add touchscreen
on Scale 2, and verify Scale 1 never disconnects. Then select Scale 2 and
complete fresh pairing without rebooting or bouncing Wi-Fi.
