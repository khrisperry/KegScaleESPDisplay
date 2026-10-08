#pragma once
#include <algorithm>
#include <cstdlib>
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
};
struct Control {
  bool pouring = false;
  const char *reason = "Boot: tap closed";
  uint32_t seen = 0, last_valid = 0, started = 0;
  int previous = 0, count = 0, low = 0, high = 0;
  void reset_ready() {
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
    if (s.sequence == seen)
      return;
    seen = s.sequence;
    if (!s.valid || (uint32_t)(now - s.at) > (uint32_t)c.stale_ms) {
      if (pouring)
        stop("LiDAR measurement invalid");
      else
        reset_ready();
      return;
    }
    if (pouring) {
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
    if (s.mm < c.min_mm || s.mm > c.max_mm || s.mm >= c.tray_mm - 20) {
      reset_ready();
      return;
    }
    if (count && (uint32_t)(s.at - last_valid) > (uint32_t)c.stale_ms)
      reset_ready();
    if (!count) {
      low = high = s.mm;
      count = 1;
    } else {
      low = std::min(low, s.mm);
      high = std::max(high, s.mm);
      if (high - low > c.stable_mm) {
        low = high = s.mm;
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
    pouring = true;
    started = now;
    reason = "Pouring";
    return true;
  }
};


} // namespace tap
