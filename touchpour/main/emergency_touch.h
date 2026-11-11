#pragma once
// Any new contact while pouring requests STOP. Consume the complete contact
// until a real all-fingers release, so it cannot navigate or start another pour.
struct EmergencyTouch {
  bool down = false, consumed = false;
  bool update(bool healthy, unsigned count, bool pouring) {
    if (!healthy) {
      consumed = true;
      return pouring; // A failed touch read cannot leave emergency control trusted.
    }
    if (!count) { down = consumed = false; return false; }
    const bool stop = pouring && !down;
    down = true;
    if (stop) consumed = true;
    return stop;
  }
};
