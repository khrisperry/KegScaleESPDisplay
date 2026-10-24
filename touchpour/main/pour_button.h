#pragma once
#include "lvgl.h"
#include <initializer_list>

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
struct Split {lv_obj_t *half=nullptr,*divider=nullptr;};
inline bool half_at(int button_top,int touch_y){return touch_y>=button_top+112;}
inline Split add_half(lv_obj_t *button,lv_event_cb_t action){
  lv_obj_set_style_clip_corner(button,true,0);
  lv_obj_remove_flag(button,LV_OBJ_FLAG_SCROLLABLE);
  Split result;
  // Clip the lower face to the INNER circle so it cannot paint over the rim.
  auto face=lv_obj_create(button);lv_obj_remove_style_all(face);
  lv_obj_set_pos(face,0,0);lv_obj_set_size(face,140,140);
  lv_obj_set_style_radius(face,LV_RADIUS_CIRCLE,0);lv_obj_set_style_clip_corner(face,true,0);
  lv_obj_remove_flag(face,LV_OBJ_FLAG_CLICKABLE);lv_obj_remove_flag(face,LV_OBJ_FLAG_SCROLLABLE);
  result.half=lv_button_create(face);lv_obj_remove_style_all(result.half);
  // Parent border is 5 px: 107 inside + 5 = 112 of the 150 px outer height.
  lv_obj_set_pos(result.half,0,107);lv_obj_set_size(result.half,140,33);
  lv_obj_set_style_bg_opa(result.half,LV_OPA_COVER,0);
  lv_obj_set_style_bg_color(result.half,lv_color_hex(0x176e91),0);
  lv_obj_set_style_bg_grad_color(result.half,lv_color_hex(0x07354d),0);
  lv_obj_set_style_bg_grad_dir(result.half,LV_GRAD_DIR_VER,0);
  lv_obj_set_style_bg_color(result.half,lv_color_hex(0x074569),LV_STATE_PRESSED);
  lv_obj_set_style_bg_color(result.half,lv_color_hex(0x284352),LV_STATE_DISABLED);
  lv_obj_set_style_bg_grad_color(result.half,lv_color_hex(0x172c39),LV_STATE_DISABLED);
  lv_obj_remove_flag(result.half,LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_event_cb(result.half,action,LV_EVENT_CLICKED,nullptr);
  auto caption=lv_label_create(result.half);lv_label_set_text(caption,"1/2 POUR");
  lv_obj_set_style_text_font(caption,&lv_font_montserrat_14,0);
  lv_obj_set_style_text_color(caption,lv_color_hex(0xf2f6f8),0);lv_obj_center(caption);
  result.divider=lv_obj_create(face);lv_obj_remove_style_all(result.divider);
  lv_obj_set_pos(result.divider,0,105);lv_obj_set_size(result.divider,140,3);
  lv_obj_set_style_bg_color(result.divider,lv_color_hex(0x07243a),0);
  lv_obj_set_style_bg_opa(result.divider,LV_OPA_COVER,0);
  lv_obj_remove_flag(result.divider,LV_OBJ_FLAG_CLICKABLE);
  lv_obj_remove_flag(result.divider,LV_OBJ_FLAG_SCROLLABLE);
  return result;
}
inline void split_state(const Split &split,bool pouring){
  for(auto object:{split.half,split.divider}){
    if(pouring)lv_obj_add_flag(object,LV_OBJ_FLAG_HIDDEN);
    else lv_obj_remove_flag(object,LV_OBJ_FLAG_HIDDEN);
  }
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
