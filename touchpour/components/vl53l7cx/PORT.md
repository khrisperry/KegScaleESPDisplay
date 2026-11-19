# VL53L7CX native ESP-IDF port

ST VL53L7CX ULD 2.0.0 API and firmware buffers obtained from RJRP44/VL53L7CX-Library (see SOURCE.md for pinned revision). Original license retained in LICENSE. No Arduino runtime or new external dependency is used.

Only the core API/firmware buffers are built. The local platform.c uses the existing external sensor I²C device handle, 16-bit register addressing, bounded 240-byte transfers, immediate error propagation, FreeRTOS waits and the ULD's 32-bit buffer endian conversion. One target/zone is requested. Data-ready is polled at 10 Hz in 8×8 mode; target statuses 5 and 9 with nonzero target count are accepted.

Do not call the upstream address-change/reset/platform-init APIs: bus ownership and the fixed 0x29 address belong to TouchPour. Interrupt, LPn and reset GPIOs are not driven by this port. Check the breakout's enable/power requirements.
