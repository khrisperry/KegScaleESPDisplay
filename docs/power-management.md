# Low-power wake behavior - V1.3.4

Touch and scheduled check-ins are independent.

- Touch draws the existing acknowledgment, then deep-sleeps for 10 seconds.
  Only the timer is armed during this delay, preventing held-touch wake loops.
- The delayed wake fetches once. A stable unchanged result ends immediately.
  An unstable calibrated result sleeps another 5 seconds and fetches once more.
  There is no third settling check; the previous image is retained if still unstable.
- Force refresh and unpair commands take precedence over the settling retry.
- Scheduled wakes fetch once, without the acknowledgment or pour delay.
- Failed normal fetches do not immediately reconnect or scan. Retry timers are
  5 minutes, 15 minutes, then hourly. Touch remains available between attempts.
  Pairing validation retains its existing single retry.
- New scale configurations default to hourly safety checks. Existing saved
  3-minute settings are preserved; disable that option in Settings to save power.
- Touch fetches skip OTA offers. Firmware offers are checked on scheduled wakes
  and physical restarts. An approved OTA may therefore wait until the next
  scheduled check; touching the display alone does not accelerate it.

## BLE optimization

RTC memory retains handles bound to the saved scale address and identity.
Touch fetches validate the scale protocol/firmware identity before using cached
handles. Invalid identity, snapshot, configuration, or control reads trigger one
rediscovery within the existing connection. Scheduled maintenance always
rediscovers. No bonding or encryption requirements are weakened.

The small display configuration packet is still read on each fetch to obtain its
revision and flags. Unchanged keg names and touch thresholds are reused.
Battery measurement and authenticated status reporting remain part of each fetch.
Cached battery/OTA commands are never replayed.

E-paper change detection, partial updates, periodic full refresh, and the
hourly safety timer are preserved. Reboots during the short delay cancel the
pending touch sequence and perform a normal maintenance check.

## Tests and hardware verification

Run: gcc -Wall -Wextra -Werror tests/power_policy_test.c -o /tmp/power_policy_test
then /tmp/power_policy_test. This exhaustively checks the bounded touch-retry
decision and the failure backoff policy.

On hardware verify: stable tap (one check-in), unstable tap (at most two),
held finger (no delay wake loop), timed check, scale disconnected (backoff),
scale firmware upgrade (cache rediscovery), force refresh/unpair, and OTA.
Measure current/awake time to quantify battery savings; no battery-life duration
is claimed without those measurements.

## Touch sensitivity

Guided touch calibration is removed. Automatic untouched-baseline measurement
before sleep remains active. New/default sensitivity is 1% below baseline;
existing Scale-saved values are preserved. Adjust sensitivity under Settings >
Displays on the Scale webpage. Current firmware clears an interrupted legacy
wizard marker rather than resuming the old flow.

The lower-level calibration routine remains in the component for compatibility
and host regression coverage, but the application no longer invokes it.

During OTA, the Wi-Fi DHCP hostname is `KegScaleDisplay-XXXX`, where `XXXX`
is the final two station MAC bytes. It is reapplied at station start before
connection/DHCP. The display normally uses BLE, so a router may retain the
previous lease name until the next Wi-Fi OTA connection; controller aliases
are independent of the device's DHCP hostname.
