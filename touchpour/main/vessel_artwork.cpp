#include "vessel_artwork.h"
#include <algorithm>
#include <cmath>

namespace vessel_artwork {
namespace {
lv_obj_t *container(lv_obj_t *parent, int x, int y, int width, int height) {
  lv_obj_t *object = lv_obj_create(parent);
  lv_obj_remove_style_all(object);
  lv_obj_set_pos(object, x, y);
  lv_obj_set_size(object, width, height);
  lv_obj_remove_flag(object, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_remove_flag(object, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_remove_flag(object, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
  lv_obj_add_flag(object, LV_OBJ_FLAG_GESTURE_BUBBLE);
  return object;
}
lv_obj_t *image(lv_obj_t *parent, const Layer &layer) {
  lv_obj_t *object = lv_image_create(parent);
  lv_image_set_src(object, layer.image);
  lv_obj_set_pos(object, layer.x, layer.y);
  lv_obj_set_size(object, layer.image->header.w, layer.image->header.h);
  lv_obj_remove_flag(object, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_flag(object, LV_OBJ_FLAG_GESTURE_BUBBLE);
  return object;
}
} // namespace

View create(lv_obj_t *parent, Kind kind, int x, int y) {
  View view;
  view.asset = &assets[static_cast<unsigned>(kind)];
  const Asset &asset = *view.asset;
  view.root = container(parent, x, y, asset.width, asset.height);
  image(view.root, asset.base);
  // Alpha is baked into the original vessel shape. Cropping these images,
  // rather than resizing them, preserves the shoulders, taper and reflections.
  view.liquid_clip = container(view.root, 0, 0, asset.width, asset.height);
  view.liquid_image = image(view.liquid_clip, asset.liquid);
  view.foam_clip = container(view.root, 0, 0, asset.width, asset.height);
  view.foam_image = image(view.foam_clip, asset.foam);
  if (asset.curved_surface)
    lv_obj_set_style_bitmap_mask_src(view.foam_clip, asset.surface_mask, 0);
  image(view.root, asset.front);
  update(view, false, 0);
  return view;
}

void update(View &view, bool ready, float percent) {
  if (!view.root || !view.asset) return;
  const Asset &asset = *view.asset;
  const float clamped = ready && std::isfinite(percent)
                            ? std::clamp(percent, 0.0f, 100.0f) : 0.0f;
  const int height = static_cast<int>(std::lround(
      (asset.bottom - asset.top) * clamped / 100.0f));
  if (height == view.last_height) return;
  view.last_height = static_cast<int16_t>(height);
  if (height <= 0) {
    lv_obj_add_flag(view.liquid_clip, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(view.foam_clip, LV_OBJ_FLAG_HIDDEN);
    return;
  }
  const int top = asset.bottom - height;
  lv_obj_set_pos(view.liquid_clip, 0, top);
  lv_obj_set_size(view.liquid_clip, asset.width, height);
  lv_obj_set_pos(view.liquid_image, asset.liquid.x, asset.liquid.y - top);
  if (asset.curved_surface) {
    // The ellipse is a small fixed image that moves with the reading. Its rear
    // half rises above the body's rectangular crop; the curved foreground
    // seam masks its lower half at near-empty levels and preserves the base.
    // Reuse the liquid image's A8 plane as a fixed body mask. This also clips
    // the moving surface at nearly empty/full levels and antialiased edges.
    lv_obj_set_pos(view.foam_clip, asset.liquid.x, asset.liquid.y);
    lv_obj_set_size(view.foam_clip, asset.liquid.image->header.w, asset.liquid.image->header.h);
    lv_obj_set_pos(view.foam_image, asset.foam.x - asset.liquid.x,
                   asset.foam.y + top - asset.top - asset.liquid.y);
  } else {
    lv_obj_set_pos(view.foam_clip, 0, top);
    lv_obj_set_size(view.foam_clip, asset.width, std::min<int>(height, asset.foam_height));
    lv_obj_set_pos(view.foam_image, asset.foam.x, asset.foam.y - top);
  }
  lv_obj_remove_flag(view.liquid_clip, LV_OBJ_FLAG_HIDDEN);
  lv_obj_remove_flag(view.foam_clip, LV_OBJ_FLAG_HIDDEN);
}
} // namespace vessel_artwork
