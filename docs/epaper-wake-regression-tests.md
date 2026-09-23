# E-paper wake and state regression tests

Run `python3 tests/run_wake_tests.py` or `bash tests/run_host_tests.sh` from the
Display repository in Linux/WSL. Requires Python 3 and a C compiler.

The runner compiles complete production functions extracted from `main/main.c`
and `components/display_ui/display_ui.c`, with their retained-state definition
and policy constants. Sleep, radio, storage, and panel operations use host fakes.

Coverage:

- Touch/timer/reset wake classification, including simultaneous touch and timer.
- Actual wake-source configuration across all 256 failure-counter values, with
  frequent check-in enabled and disabled. The safety timer remains armed.
- Timer-only pour delay clears touch wake to prevent a held-finger wake loop.
- Existing power-policy tests exhaustively cover the single unstable-pour retry.
- Stable-image preservation during movement; forced refresh bypasses suppression.
- Weight, sequence, profile, display settings, servings, and battery changes.
- Failed drawing preserves retained state so the next check can retry.
- Offline-screen threshold, failed-screen retry, failure-counter saturation,
  no repeated offline redraw, and restoration after contact resumes.
- First/forced full refresh versus changed-region refresh, with the production
  periodic full-refresh interval passed to the panel driver.
- Touch indicator clearing only when it was visible and no replacement was drawn.
- Post-pair fetch retries once; incompatible peers and failed persistence reject.
- Lightweight touch-result fetch versus normal fetch and successful contact reset.
- Unpair request clears local retained state and invokes pairing setup.

These tests do not execute the whole boot loop, BLE GATT transport, physical
panel driver, or real NVS persistence. In particular, unpair cleanup failure and
interrupted pairing across a physical reboot still require acceptance testing.

## Command delivery limitation

The Scale's `display_control_access` marks unpair delivery and clears pending
refresh/calibration flags after successfully appending a BLE read response.
That is not an acknowledgement that the display completed the operation. These
tests verify display-side command handling, not end-to-end command completion.
Command IDs, completion acknowledgements, and retry after interrupted delivery
remain the hardening plan's separate reliable-command enhancement.

## Hardware acceptance

1. Leave the Scale unavailable across repeated scheduled checks. Verify bounded
   retries, the offline screen, and recovery when the Scale returns.
2. Tap and hold the display during a pour. Verify no rapid wake loop, a bounded
   settling retry, and a settled final weight.
3. Change a profile and request full refresh. Verify the display updates at its
   next check-in and the forced refresh visibly completes.
4. Interrupt pairing, then retry. Remove and re-pair the display; confirm the old
   Scale does not remain shown as paired.
5. Repeat with frequent check-in disabled to verify touch and safety-timer access.

Local host tests passed September 22, 2026. No firmware behavior was changed by
this test increment, and nothing was flashed or published.
