#pragma once
#include "control.h"
#include <cstddef>
#include <cstdint>
#include <atomic>

namespace tap {
struct PourPoint {
  uint32_t elapsed_ms, sequence, age_ms;
  int32_t mm;
  int16_t status, servo_us;
  uint8_t valid, manual_hold, button, reserved;
};
struct PourSummary {
  uint32_t id, duration_ms, samples;
  bool manual_hold;
  char firmware[32], reason[96];
  Settings settings;
};
extern std::atomic<bool> history_saving;
void history_init();
void history_observe(const Sample &, const Settings &, uint32_t now, int servo,
                     bool pouring, bool manual_hold, bool button, const char *reason);
size_t history_list(PourSummary out[5]);
bool history_get(uint32_t id, PourSummary &out);
bool history_point(uint32_t id, size_t index, PourPoint &out);
bool history_available();


} // namespace tap
