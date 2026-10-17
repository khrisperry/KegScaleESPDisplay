#pragma once
#include "lvgl.h"
#include <cstdint>

namespace vessel_artwork {
enum class Kind : uint8_t { Can, Pint, Cup, Crowler, Growler, Keg };
struct Layer {
  const lv_image_dsc_t *image;
  int16_t x, y;
};
struct Asset {
  Layer base, liquid, foam, front;
  int16_t width, height, top, bottom, foam_height;
};
extern const Asset assets[];
// LVGL owns every object. Reset this non-owning handle when its home tree is
// deleted; no decoded images, caches or heap allocations survive a view change.
struct View {
  lv_obj_t *root = nullptr;
  lv_obj_t *liquid_clip = nullptr;
  lv_obj_t *liquid_image = nullptr;
  lv_obj_t *foam_clip = nullptr;
  lv_obj_t *foam_image = nullptr;
  const Asset *asset = nullptr;
  int16_t last_height = -1;
};
View create(lv_obj_t *parent, Kind kind, int x, int y);
void update(View &view, bool ready, float percent);
} // namespace vessel_artwork
