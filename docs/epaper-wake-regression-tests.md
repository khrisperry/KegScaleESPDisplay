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
- Reliable full-refresh completion ACK after a successful panel update.
- Failed full-refresh ACK leaves the Scale command retryable.
- Unpair ACK failure preserves the local bond; successful ACK allows local bond
  removal, retained-state cleanup, and pairing setup.

These tests do not execute the whole boot loop, BLE GATT transport, physical
panel driver, or real NVS persistence. In particular, unpair cleanup failure and
interrupted pairing across a physical reboot still require acceptance testing.

## Reliable command completion

Scale and e-paper V1.3.5 use a non-zero command ID in the existing 4-byte display
control packet plus a separate authenticated/encrypted completion-ACK
characteristic. The Scale retains V1.3.5+ commands until a matching completion
ACK is accepted.

The wake host harness covers the display-side completion boundary:

- Legacy/no-ACK commands continue to work without a completion write.
- A forced refresh writes its ACK only after the panel update succeeds.
- An ACK transport failure does not turn a completed panel update into a false
  Scale-side completion; the Scale can offer the still-pending command again.
- An unpair command must ACK successfully before the display deletes its bond.
- A failed unpair ACK leaves the local pairing intact for a later retry.
- A successful unpair ACK permits bond removal, retained-state cleanup, and
  transition back to pairing mode.

The Scale host suite separately guards command-ID allocation, matching of ACK IDs
and flags, authenticated characteristic wiring, legacy fallback, and pending-state
telemetry. Real BLE timing and interrupted power between panel completion and the
ACK write remain hardware acceptance scenarios.

## Hardware acceptance

1. Leave the Scale unavailable across repeated scheduled checks. Verify bounded
   retries, the offline screen, and recovery when the Scale returns.
2. Tap and hold the display during a pour. Verify no rapid wake loop, a bounded
   settling retry, and a settled final weight.
3. Change a profile and request full refresh. Verify the Scale reports a pending
   command ID before the wake, the panel performs a full refresh, the Scale logs
   the matching completion ACK, and the pending command clears afterward.
4. Interrupt power after a refresh is requested but before completion/ACK. Verify
   the command remains pending and is offered again on a later wake.
5. Request Remove display. Verify the display acknowledges before deleting its
   bond, the Scale clears its pairing on disconnect, and both sides return to the
   expected unpaired state.
6. Interrupt pairing, then retry. Remove and re-pair the display; confirm the old
   Scale does not remain shown as paired.
7. Repeat with frequent check-in disabled to verify touch and safety-timer access.

V1.3.5 Dev adds reliable command completion behavior. The host suites cover the
retry and state-transition rules; the physical BLE timing scenarios above must be
validated before promotion to production.
