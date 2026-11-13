#pragma once
#include "control.h"

namespace tap {
struct ButtonEvent { bool pressed = false, released = false, short_press = false, long_press = false; };
class ButtonGesture {
public:
  static constexpr uint32_t debounce_ms = 30, hold_ms = 2000;
  ButtonGesture(bool high, uint32_t now) : raw_previous(high), debounced(high), armed(high), blocked(!high), edge(now) {}
  void cancelUntilRelease() { blocked = true; }
  ButtonEvent update(bool high, uint32_t now, bool busy) {
    ButtonEvent e;
    if (high != raw_previous) { raw_previous = high; edge = now; }
    if (now - edge >= debounce_ms && high != debounced) {
      debounced = high;
      if (high) {
        e.released = true;
        e.short_press = armed && !blocked && !long_sent;
        armed = true; blocked = false; long_sent = false;
      } else if (armed) {
        e.pressed = true; pressed_at = now; blocked = busy; long_sent = false;
      }
    }
    if (!high && !debounced && armed && !blocked && !long_sent && now - pressed_at >= hold_ms) {
      e.long_press = true; long_sent = true;
    }
    return e;
  }
private:
  bool raw_previous, debounced, armed, blocked, long_sent = false;
  uint32_t edge, pressed_at = 0;
};


} // namespace tap
