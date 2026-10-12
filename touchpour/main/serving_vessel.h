#pragma once
#include "lvgl.h"
#include <cmath>

namespace serving_vessel {
enum class Kind { Pint, Solo, Can };
inline Kind for_serving(float ounces) {
  if (std::fabs(ounces - 12.0f) <= 0.25f) return Kind::Can;
  if (std::fabs(ounces - 20.0f) <= 0.25f) return Kind::Solo;
  // No crowler/growler artwork on the tap handle.
  return Kind::Pint;
}
// Scaled from touchscreen/main/home_layout.cpp: the same 174x280 tapered
// silhouettes, gray outline, amber fill, can seam, and Solo rim/base.
inline lv_obj_t *create(lv_obj_t *parent, Kind kind, bool has_reading, float percent) {
  auto vessel = lv_obj_create(parent);
  lv_obj_remove_style_all(vessel);
  lv_obj_set_size(vessel, 40, 60);
  lv_obj_remove_flag(vessel, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_remove_flag(vessel, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_flag(vessel, LV_OBJ_FLAG_GESTURE_BUBBLE);
  auto rectangle = [&](int x, int y, int w, int h, uint32_t color, int radius=0) {
    auto object = lv_obj_create(vessel);
    lv_obj_remove_style_all(object);
    lv_obj_set_pos(object,x,y); lv_obj_set_size(object,w,h);
    lv_obj_set_style_bg_color(object,lv_color_hex(color),0);
    lv_obj_set_style_bg_opa(object,LV_OPA_COVER,0);
    lv_obj_set_style_radius(object,radius,0);
    lv_obj_remove_flag(object,LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(object,LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(object,LV_OBJ_FLAG_GESTURE_BUBBLE);
    return object;
  };
  int filled = has_reading ? int(std::fmax(0,std::fmin(100,percent))*56/100) : 0;
  for (int y=58-filled; y<58; ++y) {
    int inset = kind==Kind::Can ? 6 : 3 + (kind==Kind::Solo ? 5 : 4)*y/58;
    rectangle(inset,y,40-2*inset,1,0xd58b12);
  }
  if (kind==Kind::Can) {
    auto outline=rectangle(4,0,32,60,0x98a8b0,4);
    lv_obj_set_style_bg_opa(outline,LV_OPA_TRANSP,0);
    lv_obj_set_style_border_color(outline,lv_color_hex(0x98a8b0),0);
    lv_obj_set_style_border_width(outline,2,0);
    rectangle(7,3,26,1,0x98a8b0);
  } else {
    static lv_point_precise_t pint[]={{2,1},{38,1},{33,59},{7,59},{2,1}};
    static lv_point_precise_t solo[]={{2,1},{38,1},{33,59},{7,59},{2,1}};
    auto outline=lv_line_create(vessel);
    lv_line_set_points(outline,kind==Kind::Solo?solo:pint,5);
    lv_obj_set_style_line_color(outline,lv_color_hex(0x98a8b0),0);
    lv_obj_set_style_line_width(outline,2,0);
    lv_obj_set_style_line_rounded(outline,true,0);
    lv_obj_remove_flag(outline,LV_OBJ_FLAG_CLICKABLE);
    if (kind==Kind::Solo) {
      auto rim=rectangle(5,3,30,3,0x98a8b0,2);
      lv_obj_set_style_bg_opa(rim,LV_OPA_TRANSP,0);
      lv_obj_set_style_border_color(rim,lv_color_hex(0x98a8b0),0);
      lv_obj_set_style_border_width(rim,1,0);
      rectangle(12,56,16,1,0x98a8b0);
    }
  }
  return vessel;
}
} // namespace serving_vessel
