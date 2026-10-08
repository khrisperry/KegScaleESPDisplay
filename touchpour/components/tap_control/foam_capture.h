#pragma once
#include "control.h"
#include <cstddef>
#include <cstdint>

namespace tap {

struct FoamRow {
  uint32_t elapsed_ms, sequence, age_ms, interval_ms;
  int32_t mm, delta_mm;
  int16_t status, servo_us, cutoff_mm, tray_mm;
  uint8_t flags, markers, stable_samples;
  const char *reason;
};
class FoamCapture {
public:
  static constexpr size_t capacity = 3000;
  static constexpr uint32_t duration_ms = 300000;
  enum Flag { VALID = 1, DELTA_VALID = 2, POURING = 4, READY = 8,
              SERVO_TEST = 16, BUTTON = 32, MANUAL_HOLD = 64 };
  enum Marker { SETTLING = 1, CUP_REMOVED = 2, CUP_RETURNED = 4 };
  FoamRow rows[capacity]{};
  size_t count = 0;
  bool active = false;
  uint32_t started = 0, ended = 0;
  void start(uint32_t now) {
    count = 0; started = ended = now; active = true;
    seen = false; previous_valid = false; pending_button = false; pending_markers = 0;
  }
  void stop(uint32_t now) { if (active) { active = false; ended = now; } }
  uint32_t elapsed(uint32_t now) const { return (active ? now : ended) - started; }
  bool mark(uint8_t marker) {
    if (!active) return false;
    pending_markers |= marker; return true;
  }
  void observe(const Sample &s, const Settings &c, uint32_t now, int servo,
               bool pouring, bool ready, bool test, bool button, const char *reason) {
    if (!active) return;
    if (now - started >= duration_ms || count == capacity) { stop(now); return; }
    pending_button |= button;
    if ((int32_t)(s.at - started) < 0 || (seen && s.sequence == last_sequence)) return;
    seen = true; last_sequence = s.sequence;
    bool delta_valid = s.valid && previous_valid;
    uint32_t interval = count ? s.at - previous_at : 0;
    uint8_t flags = (s.valid ? VALID : 0) | (delta_valid ? DELTA_VALID : 0) |
                    (pouring ? POURING : 0) | (ready ? READY : 0) |
                    (test ? SERVO_TEST : 0) | (pending_button ? BUTTON : 0);
    rows[count++] = {s.at - started, s.sequence, now - s.at, interval,
                     s.mm, delta_valid ? s.mm - previous_mm : 0,
                     (int16_t)s.status, (int16_t)servo, (int16_t)c.stop_mm,
                     (int16_t)c.tray_mm, flags, pending_markers, 0, reason};
    previous_valid = s.valid; previous_at = s.at; previous_mm = s.mm;
    pending_button = false; pending_markers = 0;
    if (count == capacity) stop(now);
  }
private:
  bool seen = false, previous_valid = false, pending_button = false;
  uint32_t last_sequence = 0, previous_at = 0;
  int previous_mm = 0;
  uint8_t pending_markers = 0;
};
struct FoamStatus { bool active; size_t count; uint32_t elapsed_ms; };
void foam_init();
void foam_start(uint32_t now);
void foam_stop(uint32_t now);
bool foam_mark(uint8_t marker);
FoamStatus foam_status(uint32_t now);
bool foam_row(size_t index, FoamRow &out);
void foam_observe(const Sample &, const Settings &, uint32_t now, int servo,
                  bool pouring, bool ready, bool test, bool button, int stable,
                  const char *reason, bool manual_hold);


} // namespace tap
