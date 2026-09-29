#include "ui_lifetime.h"

#include <cassert>
#include <cstdint>
#include <cstdio>

int main() {
  TouchscreenUiGeneration generation{1};

  assert(touchscreen_ui_generation_current(&generation) == 1);
  assert(touchscreen_ui_generation_accepts(&generation, 1));
  assert(!touchscreen_ui_generation_accepts(&generation, 0));
  assert(!touchscreen_ui_generation_accepts(&generation, 2));

  assert(touchscreen_ui_generation_advance(&generation) == 2);
  assert(!touchscreen_ui_generation_accepts(&generation, 1));
  assert(touchscreen_ui_generation_accepts(&generation, 2));

  generation.value = UINT32_MAX;
  assert(touchscreen_ui_generation_advance(&generation) == 1);
  assert(touchscreen_ui_generation_accepts(&generation, 1));
  assert(!touchscreen_ui_generation_accepts(&generation, UINT32_MAX));

  std::puts("PASS: Touch UI generation lifetime regression");
  return 0;
}
