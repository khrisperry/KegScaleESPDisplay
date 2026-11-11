# Diagnostic firmware — manual installation only

This image adds instrumentation, not a claimed connection fix. It does not publish
an OTA feed. Existing partitions, pairing, calibration, and protocol are retained.
Always match the hardware to BUILD_IDENTITY.json. The 4.3-inch image is not the
square 4B image. TouchPour must be installed locally with the tap linkage
disconnected, and its calibrated closed position and emergency stop checked before
reconnecting the mechanism. Its staged V0.1.20 maintenance safeguards are retained.

## Capture

Use the existing serial monitors at 115200 baud, computer timestamps enabled,
and Ctrl+T then Ctrl+L to save output. Keep the computer awake. Expect a
`flight_recorder: HEALTH` line every 30 seconds on always-on devices. Touchscreen
and TouchPour also print QUEUE_HEALTH every 30 seconds once their app loop runs.
The sleeping e-Ink retains eight BLE fetch outcomes in RTC memory across deep sleep and prints wake, BLE fetch and sleep diagnostics; no heartbeat is
expected during deep sleep. Do not prevent sleep to collect logs.

Download scale support bundles before and after each controlled test. Their
`diagnostic_events` array is independent of the general log buffer. Bundles also contain `diagnostic_allocations`, frozen-state/failure counters, and the first failed allocation with its six call sites, so the scale evidence is retrievable without USB. Save both
serial logs and this artifact's ELF, map, sdkconfig, checksums and build identity.
The static recorder reports its RAM footprint at startup. Instrumentation consumes
RAM and CPU; compare the diagnostic baseline separately from V1.4.20.

The recorder retains 24 event snapshots and 24 recent allocations >=1024 bytes,
with task handles, free observations, and six raw backtrace PCs. This is a bounded
candidate history, NOT complete allocation/leak accounting. Smaller allocations
and overwritten records are not represented. Hooks do not print, allocate, write
flash, or acquire locks. Free observations and multi-core traces are best effort.
A failed allocation captures its size, capability flags and call sites. The first
failure and the candidate history freeze; a new minimum below 8 KiB after the
first minute also freezes candidates. A 100ms timer observes historical minima,
so a transient dip is detected even if free memory already recovered; this does
not make the observation timestamp the exact allocation timestamp. Startup lows
are recorded but do not trigger the post-startup freeze.

Frozen records dump to serial after a trigger (rate limited), from timer context,
not the allocation hook. On a scale, authenticated POST /api/diagnostics/dump queues
a serial dump; POST /api/diagnostics/rearm resumes candidates AFTER evidence is
saved. Authentication follows the existing admin policy. These endpoints do not
change the automatic OTA check schedule.

## Controlled tests

1. Home scale and 4.3-inch: idle capture, then an OTA feed check with logging on.
2. Download a support bundle and visit settings; note times. Compare HTTP/activity
   boundary events and minima against idle operation.
3. Interrupt ONLY the display's Wi-Fi, restore it, and verify authentication,
   stale-session retirement and continued weight updates. Note the exact times.
4. Capture an overnight run. Do not change AP assignments or firmware mid-test.
5. After the home baseline is understood, repeat scale/e-Ink capture at the cottage.
   Install TouchPour only under local supervision; do not intentionally interrupt
   communications or run maintenance tests during a pour.

Each package includes its exact ELF. Decode raw `pc=...` addresses using the
matching toolchain's addr2line: xtensa-esp32s3-elf-addr2line for S3, and
xtensa-esp32-elf-addr2line for the e-Ink. Example:

    xtensa-esp32s3-elf-addr2line -pfiaC -e <matching.elf> 0x42001234 0x42005678

Do not use a rebuilt or different-version ELF. The diagnostic workflow only builds
and uploads artifacts; signing/promotion into Dev/Beta/Production is separate.
