#pragma once

#include <stdint.h>

struct TouchscreenUiGeneration {
  uint32_t value;
};

static inline uint32_t touchscreen_ui_generation_current(
    const TouchscreenUiGeneration *generation) {
  return generation ? generation->value : 0;
}

static inline uint32_t touchscreen_ui_generation_advance(
    TouchscreenUiGeneration *generation) {
  if (!generation)
    return 0;
  ++generation->value;
  if (generation->value == 0)
    generation->value = 1;
  return generation->value;
}

static inline bool touchscreen_ui_generation_accepts(
    const TouchscreenUiGeneration *generation, uint32_t request_generation) {
  return generation && request_generation != 0 &&
         request_generation == generation->value;
}
