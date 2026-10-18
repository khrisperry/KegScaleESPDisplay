#pragma once
#include <algorithm>
#include <cstdlib>
#include <cmath>
#include <stdint.h>

namespace tap {
struct Settings {
  uint32_t version = 1;
  int closed_us = 1500, open_us = 1600, stop_mm = 80, min_mm = 100,
      max_mm = 450, tray_mm = 600;
  int stable_mm = 15, jump_mm = 80, stale_ms = 350, max_pour_ms = 30000;
  bool servo_calibrated = false, distance_calibrated = false;
};
inline bool valid_settings(const Settings &c) {
  return c.version == 1 && c.closed_us >= 500 && c.closed_us <= 2500 &&
         c.open_us >= 500 && c.open_us <= 2500 &&
         (!c.servo_calibrated || c.closed_us != c.open_us) && c.stop_mm >= 20 &&
         c.stop_mm < c.min_mm && c.min_mm < c.max_mm && c.tray_mm >= 50 &&
         c.tray_mm <= 2000 && c.max_mm < c.tray_mm - 20 && c.stable_mm >= 1 &&
         c.stable_mm <= 100 && c.jump_mm >= 10 && c.jump_mm <= 500 &&
         c.stale_ms >= 150 && c.stale_ms <= 500 && c.max_pour_ms >= 1000 &&
         c.max_pour_ms <= 120000;
}
struct Sample {
  int mm = 0, status = 0;
  bool valid = false;
  uint32_t at = 0, sequence = 0;
  float filtered_mm = 0;
  bool filtered_valid = false;
  int stable_distance() const {
    return filtered_valid ? int(std::lround(filtered_mm)) : mm;
  }
};
struct Control {
  bool pouring = false;
  // One no-return sentinel may occur as the opening servo starts moving.
  static constexpr uint32_t startup_dropout_window_ms = 250;
  static constexpr uint32_t startup_recovery_ms = 350;
  bool startup_dropout_used = false, startup_dropout_pending = false;
  const char *reason = "Boot: tap closed";
  uint32_t seen = 0, last_valid = 0, started = 0;
  int previous = 0, count = 0, low = 0, high = 0;
  void reset_ready() {
    startup_dropout_used = startup_dropout_pending = false;
    count = 0;
    previous = 0;
    last_valid = 0;
  }
  void stop(const char *why) {
    pouring = false;
    reason = why;
    reset_ready();
  }
  bool ready(const Settings &c, uint32_t now) const {
    return c.servo_calibrated && c.distance_calibrated && count >= 5 &&
           (uint32_t)(now - last_valid) <= (uint32_t)c.stale_ms;
  }
  void update(const Settings &c, const Sample &s, uint32_t now) {
    if (pouring && (uint32_t)(now - started) >= (uint32_t)c.max_pour_ms) {
      stop("Maximum pour time");
      return;
    }
    if (pouring && (uint32_t)(now - last_valid) > (uint32_t)c.stale_ms) {
      stop("LiDAR measurement stale");
      return;
    }
    if (pouring && startup_dropout_pending &&
        uint32_t(now - started) >= startup_recovery_ms) {
      stop("LiDAR startup recovery timeout");
      return;
    }
    if (s.sequence == seen)
      return;
    seen = s.sequence;
    if (!s.valid || (uint32_t)(now - s.at) > (uint32_t)c.stale_ms) {
      if (pouring) {
        // Do not treat an invalid 65535 sentinel as a real distance/jump.
        // Never renew last_valid or extend the existing stale-data deadline.
        // A second invalid, a late fault or an old sample still closes at once.
        if (!startup_dropout_used && !s.valid && s.mm == 65535 &&
            uint32_t(now - started) < startup_dropout_window_ms &&
            uint32_t(now - s.at) <= uint32_t(c.stale_ms)) {
          startup_dropout_used = startup_dropout_pending = true;
          return;
        }
        stop("LiDAR measurement invalid");
      } else
        reset_ready();
      return;
    }
    if (pouring) {
      startup_dropout_pending = false;
      // A close-range sample must never be filtered away as an outlier.
      if (s.mm <= c.stop_mm) {
        stop("Fill target reached");
        return;
      }
      if (s.mm > c.max_mm || s.mm >= c.tray_mm - 20) {
        stop("Cup removed or distance out of range");
        return;
      }
      if (previous && abs(s.mm - previous) > c.jump_mm) {
        stop("Erratic LiDAR readings");
        return;
      }
      previous = s.mm;
      last_valid = s.at;
      return;
    }
    // A partial cup is closer than the empty-cup calibration reference.
    // Allow top-ups throughout the usable fill range, but require both raw
    // and smoothed distance above the cutoff before collecting readiness.
    if (s.stable_distance() <= c.stop_mm || s.stable_distance() > c.max_mm ||
        s.mm <= c.stop_mm || s.mm >= c.tray_mm - 20) {
      reset_ready();
      return;
    }
    if (count && (uint32_t)(s.at - last_valid) > (uint32_t)c.stale_ms)
      reset_ready();
    if (!count) {
      low = high = s.stable_distance();
      count = 1;
    } else {
      low = std::min(low, s.stable_distance());
      high = std::max(high, s.stable_distance());
      if (high - low > c.stable_mm) {
        low = high = s.stable_distance();
        count = 1;
      } else
        count = std::min(count + 1, 5);
    }
    previous = s.mm;
    last_valid = s.at;
  }
  bool start(const Settings &c, uint32_t now) {
    if (!ready(c, now)) {
      reason = "Start rejected: calibration or stable cup reading required";
      return false;
    }
    startup_dropout_used = startup_dropout_pending = false;
    pouring = true;
    started = now;
    reason = "Pouring";
    return true;
  }
};


} // namespace tap
