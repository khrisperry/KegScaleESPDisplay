#pragma once
#ifdef KEG_TOUCHSCREEN_43
#include "bsp43.h"
#else
#include "bsp/esp32_s3_touch_lcd_4b.h"
#endif
#include "lvgl.h"

// All fixed-coordinate screens, keyboards and overlays share this viewport.
inline lv_obj_t *touchscreen_ui_root() {
#ifdef KEG_TOUCHSCREEN_43
  static lv_obj_t *viewport = nullptr;
  if (!viewport) {
    auto screen = lv_screen_active();
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x101c26), 0);
    lv_obj_remove_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
    viewport = lv_obj_create(screen);
    lv_obj_set_size(viewport, 480, 480);
    lv_obj_center(viewport);
    lv_obj_set_style_pad_all(viewport, 0, 0);
    lv_obj_set_style_border_width(viewport, 0, 0);
    lv_obj_set_style_radius(viewport, 0, 0);
    lv_obj_remove_flag(viewport, LV_OBJ_FLAG_SCROLLABLE);
  }
  return viewport;
#else
  return lv_screen_active();
#endif
}
