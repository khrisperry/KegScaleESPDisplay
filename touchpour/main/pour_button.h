#pragma once
#include "lvgl.h"

// Native LVGL gradients/bevels: no large button bitmaps or image decoder.
namespace pour_button {
inline void palette(lv_obj_t *button, bool pouring) {
  lv_obj_set_style_bg_color(button,lv_color_hex(pouring?0xff525b:0x29b5ff),0);
  lv_obj_set_style_bg_grad_color(button,lv_color_hex(pouring?0xa90019:0x0044ad),0);
  lv_obj_set_style_border_color(button,lv_color_hex(pouring?0xee727a:0x68c8ff),0);
  lv_obj_set_style_bg_color(button,lv_color_hex(pouring?0xbf1728:0x075cbd),LV_STATE_PRESSED);
  lv_obj_set_style_bg_grad_color(button,lv_color_hex(pouring?0x850013:0x002e78),LV_STATE_PRESSED);
}
inline void style(lv_obj_t *button) {
  // Own the face styles instead of inheriting animated theme styles.
  // LVGL 9.5 dereferences transition descriptors on state changes: never
  // disable a transition by assigning nullptr to LV_STYLE_TRANSITION.
  lv_obj_remove_style_all(button);
  lv_obj_set_style_radius(button,LV_RADIUS_CIRCLE,0);
  lv_obj_set_style_pad_all(button,0,0);
  lv_obj_set_style_bg_opa(button,LV_OPA_COVER,0);
  lv_obj_set_style_bg_grad_dir(button,LV_GRAD_DIR_VER,0);
  lv_obj_set_style_border_width(button,5,0);
  lv_obj_set_style_border_opa(button,LV_OPA_COVER,0);
  lv_obj_set_style_outline_width(button,2,0);
  lv_obj_set_style_outline_pad(button,0,0);
  lv_obj_set_style_outline_color(button,lv_color_hex(0x04101c),0);
  lv_obj_set_style_shadow_width(button,16,0);
  lv_obj_set_style_shadow_color(button,lv_color_hex(0x00060d),0);
  lv_obj_set_style_shadow_opa(button,LV_OPA_70,0);
  lv_obj_set_style_shadow_offset_y(button,7,0);
  lv_obj_set_style_shadow_spread(button,2,0);
  lv_obj_set_style_translate_y(button,2,LV_STATE_PRESSED);
  lv_obj_set_style_shadow_offset_y(button,3,LV_STATE_PRESSED);
  lv_obj_set_style_shadow_width(button,8,LV_STATE_PRESSED);
  // Disabled stays clearly blue, with a muted finish; readiness logic is unchanged.
  lv_obj_set_style_bg_color(button,lv_color_hex(0x397396),LV_STATE_DISABLED);
  lv_obj_set_style_bg_grad_color(button,lv_color_hex(0x183653),LV_STATE_DISABLED);
  lv_obj_set_style_border_color(button,lv_color_hex(0x527f99),LV_STATE_DISABLED);
  lv_obj_set_style_opa(button,LV_OPA_COVER,LV_STATE_DISABLED);
  lv_obj_set_style_recolor_opa(button,LV_OPA_TRANSP,LV_STATE_DISABLED);
  palette(button,false);
  auto bevel=lv_obj_create(button);
  lv_obj_remove_style_all(bevel);
  // Position inside the five-pixel outer rim; the dark inset defines the face.
  lv_obj_set_size(bevel,140,140);lv_obj_center(bevel);
  lv_obj_set_style_radius(bevel,LV_RADIUS_CIRCLE,0);
  lv_obj_set_style_border_width(bevel,2,0);
  lv_obj_set_style_border_color(bevel,lv_color_hex(0x041c39),0);
  lv_obj_set_style_border_opa(bevel,LV_OPA_70,0);
  lv_obj_remove_flag(bevel,LV_OBJ_FLAG_CLICKABLE);
  lv_obj_remove_flag(bevel,LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_move_to_index(bevel,0);
  auto shine=lv_obj_create(button);
  lv_obj_remove_style_all(shine);
  lv_obj_set_size(shine,116,58);lv_obj_set_pos(shine,12,5);
  lv_obj_set_style_radius(shine,LV_RADIUS_CIRCLE,0);
  lv_obj_set_style_bg_color(shine,lv_color_hex(0xffffff),0);
  lv_obj_set_style_bg_grad_color(shine,lv_color_hex(0xffffff),0);
  lv_obj_set_style_bg_opa(shine,LV_OPA_COVER,0);
  lv_obj_set_style_bg_main_opa(shine,LV_OPA_20,0);
  lv_obj_set_style_bg_grad_opa(shine,LV_OPA_TRANSP,0);
  lv_obj_set_style_bg_grad_dir(shine,LV_GRAD_DIR_VER,0);
  lv_obj_remove_flag(shine,LV_OBJ_FLAG_CLICKABLE);
  lv_obj_remove_flag(shine,LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_move_to_index(shine,1);
}
inline void update(lv_obj_t *button, bool pouring) {
  // Called from the UI poll; only invalidate the gradients on state changes.
  const bool previous=lv_obj_has_state(button,LV_STATE_USER_1);
  if(previous==pouring)return;
  if(pouring)lv_obj_add_state(button,LV_STATE_USER_1);
  else lv_obj_remove_state(button,LV_STATE_USER_1);
  palette(button,pouring);
}
} // namespace pour_button
