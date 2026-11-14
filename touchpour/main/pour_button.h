#pragma once
#include "lvgl.h"
#include <initializer_list>
#include <cmath>

// Native LVGL gradients/bevels: no large button bitmaps or image decoder.
//
// V0.1.23 layout: the 150 px round button is split evenly, FULL POUR on the
// top half and 1/2 POUR on the bottom half. A separate 66 px SMALL CUP button
// sits to its right. During any pour the round face becomes one red
// EMERGENCY STOP (the UI also centres it and hides SMALL CUP).
namespace pour_button {
constexpr int kBorder=5;        // outer rim of the 150 px button
constexpr int kFace=140;        // inner face (150 - 2 x 5)
constexpr int kDividerRow=69;   // three dark rows split the face at its middle
constexpr int kCapRow=72;       // first row of the 1/2 POUR half (face coords)
constexpr int kCapHeight=kFace-kCapRow;

inline void palette(lv_obj_t *button, bool pouring) {
  lv_obj_set_style_bg_color(button,lv_color_hex(pouring?0xff525b:0x33b8ff),0);
  lv_obj_set_style_bg_grad_color(button,lv_color_hex(pouring?0xa90019:0x003f9e),0);
  lv_obj_set_style_border_color(button,lv_color_hex(pouring?0xff8a90:0x7fd0ff),0);
  lv_obj_set_style_bg_color(button,lv_color_hex(pouring?0xbf1728:0x1a8ad8),LV_STATE_PRESSED);
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
  lv_obj_set_style_border_width(button,kBorder,0);
  lv_obj_set_style_border_opa(button,LV_OPA_COVER,0);
  lv_obj_set_style_outline_width(button,2,0);
  lv_obj_set_style_outline_pad(button,0,0);
  lv_obj_set_style_outline_color(button,lv_color_hex(0x04101c),0);
  lv_obj_set_style_shadow_width(button,14,0);
  lv_obj_set_style_shadow_color(button,lv_color_hex(0x00060d),0);
  lv_obj_set_style_shadow_opa(button,LV_OPA_70,0);
  lv_obj_set_style_shadow_offset_y(button,6,0);
  lv_obj_set_style_shadow_spread(button,1,0);
  lv_obj_set_style_translate_y(button,2,LV_STATE_PRESSED);
  lv_obj_set_style_shadow_offset_y(button,3,LV_STATE_PRESSED);
  lv_obj_set_style_shadow_width(button,8,LV_STATE_PRESSED);
  // Disabled stays recognisably blue with a muted finish; readiness logic is unchanged.
  lv_obj_set_style_bg_color(button,lv_color_hex(0x2f5a75),LV_STATE_DISABLED);
  lv_obj_set_style_bg_grad_color(button,lv_color_hex(0x1c3a4f),LV_STATE_DISABLED);
  lv_obj_set_style_border_color(button,lv_color_hex(0x4a6f84),LV_STATE_DISABLED);
  lv_obj_set_style_shadow_opa(button,LV_OPA_TRANSP,LV_STATE_DISABLED);
  lv_obj_set_style_opa(button,LV_OPA_COVER,LV_STATE_DISABLED);
  lv_obj_set_style_recolor_opa(button,LV_OPA_TRANSP,LV_STATE_DISABLED);
  palette(button,false);
  auto bevel=lv_obj_create(button);
  lv_obj_remove_style_all(bevel);
  // Position inside the five-pixel outer rim; the dark inset defines the face.
  lv_obj_set_size(bevel,kFace,kFace);lv_obj_center(bevel);
  lv_obj_set_style_radius(bevel,LV_RADIUS_CIRCLE,0);
  lv_obj_set_style_border_width(bevel,2,0);
  lv_obj_set_style_border_color(bevel,lv_color_hex(0x041c39),0);
  lv_obj_set_style_border_opa(bevel,LV_OPA_70,0);
  lv_obj_remove_flag(bevel,LV_OBJ_FLAG_CLICKABLE);
  lv_obj_remove_flag(bevel,LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_move_to_index(bevel,0);
  auto shine=lv_obj_create(button);
  lv_obj_remove_style_all(shine);
  lv_obj_set_size(shine,116,50);lv_obj_set_pos(shine,12,5);
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
// The whole lower half, including the bottom rim, belongs to 1/2 POUR.
inline bool half_at(int button_top,int touch_y){return touch_y>=button_top+kBorder+kCapRow;}
// Draw the half-circle directly into the display's current strip.
// Rounded child clipping creates nested ARGB layers; on the ESP32 these can
// exhaust the DMA draw-buffer heap and leave LVGL retrying forever.
inline void draw_cap(lv_event_t *event) {
  auto object=static_cast<lv_obj_t *>(lv_event_get_target(event));
  const bool divider=lv_event_get_user_data(event)!=nullptr;
  const int first_row=divider?kDividerRow:kCapRow;
  const bool disabled=lv_obj_has_state(object,LV_STATE_DISABLED);
  const bool pressed=lv_obj_has_state(object,LV_STATE_PRESSED);
  const auto top=lv_color_hex(divider?0x04182a:disabled?0x23495a:pressed?0x137391:0x1a8aa8);
  const auto bottom=lv_color_hex(divider?0x04182a:disabled?0x152f3c:0x0b4d66);
  lv_area_t coords;lv_obj_get_coords(object,&coords);
  auto layer=lv_event_get_layer(event);
  lv_draw_rect_dsc_t rect;lv_draw_rect_dsc_init(&rect);
  rect.bg_opa=LV_OPA_COVER;
  const int height=lv_area_get_height(&coords);
  const float radius=kFace/2.0f;
  for(int row=0;row<height;++row) {
    const float dy=first_row+row-(radius-0.5f);
    const float squared=radius*radius-dy*dy;
    const float extent=squared>0?std::sqrt(squared):0;
    const int inset=int(std::ceil((radius-0.5f)-extent));
    if(inset>=kFace/2)continue;
    lv_area_t line{coords.x1+inset,coords.y1+row,coords.x1+kFace-1-inset,coords.y1+row};
    rect.bg_color=lv_color_mix(bottom,top,height>1?row*255/(height-1):0);
    lv_draw_rect(layer,&rect,&line);
  }
}
inline Split add_half(lv_obj_t *button,lv_event_cb_t action){
  lv_obj_remove_flag(button,LV_OBJ_FLAG_SCROLLABLE);
  Split result;
  // Transparent carriers provide hit targets only. No corner masks/layers.
  auto face=lv_obj_create(button);lv_obj_remove_style_all(face);
  lv_obj_set_pos(face,0,0);lv_obj_set_size(face,kFace,kFace);
  lv_obj_remove_flag(face,LV_OBJ_FLAG_CLICKABLE);lv_obj_remove_flag(face,LV_OBJ_FLAG_SCROLLABLE);
  result.half=lv_button_create(face);lv_obj_remove_style_all(result.half);
  lv_obj_set_pos(result.half,0,kCapRow);lv_obj_set_size(result.half,kFace,kCapHeight);
  lv_obj_remove_flag(result.half,LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_event_cb(result.half,draw_cap,LV_EVENT_DRAW_MAIN,nullptr);
  lv_obj_add_event_cb(result.half,action,LV_EVENT_CLICKED,nullptr);
  auto caption=lv_label_create(result.half);lv_label_set_text(caption,"1/2\nPOUR");
  lv_obj_set_style_text_font(caption,&lv_font_montserrat_20,0);
  lv_obj_set_style_text_align(caption,LV_TEXT_ALIGN_CENTER,0);
  lv_obj_set_style_text_color(caption,lv_color_hex(0xffffff),0);
  lv_obj_align(caption,LV_ALIGN_CENTER,0,-6);
  result.divider=lv_obj_create(face);lv_obj_remove_style_all(result.divider);
  lv_obj_set_pos(result.divider,0,kDividerRow);lv_obj_set_size(result.divider,kFace,kCapRow-kDividerRow);
  lv_obj_add_event_cb(result.divider,draw_cap,LV_EVENT_DRAW_MAIN,result.divider);
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
// Separate SMALL CUP button: one plain circle with a label, no child clipping.
constexpr int kSmallSize=66;
inline lv_obj_t *add_small(lv_obj_t *parent,lv_event_cb_t action){
  auto button=lv_button_create(parent);
  lv_obj_remove_style_all(button);
  lv_obj_set_size(button,kSmallSize,kSmallSize);
  lv_obj_remove_flag(button,LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_style_radius(button,LV_RADIUS_CIRCLE,0);
  lv_obj_set_style_bg_opa(button,LV_OPA_COVER,0);
  lv_obj_set_style_bg_grad_dir(button,LV_GRAD_DIR_VER,0);
  lv_obj_set_style_bg_color(button,lv_color_hex(0x6f7dff),0);
  lv_obj_set_style_bg_grad_color(button,lv_color_hex(0x3a3fb8),0);
  lv_obj_set_style_border_width(button,3,0);
  lv_obj_set_style_border_color(button,lv_color_hex(0xa9b2ff),0);
  lv_obj_set_style_border_opa(button,LV_OPA_COVER,0);
  lv_obj_set_style_outline_width(button,2,0);
  lv_obj_set_style_outline_color(button,lv_color_hex(0x04101c),0);
  lv_obj_set_style_shadow_width(button,10,0);
  lv_obj_set_style_shadow_color(button,lv_color_hex(0x00060d),0);
  lv_obj_set_style_shadow_opa(button,LV_OPA_70,0);
  lv_obj_set_style_shadow_offset_y(button,5,0);
  lv_obj_set_style_bg_color(button,lv_color_hex(0x5a67e6),LV_STATE_PRESSED);
  lv_obj_set_style_bg_grad_color(button,lv_color_hex(0x2c3096),LV_STATE_PRESSED);
  lv_obj_set_style_translate_y(button,2,LV_STATE_PRESSED);
  lv_obj_set_style_shadow_offset_y(button,2,LV_STATE_PRESSED);
  lv_obj_set_style_bg_color(button,lv_color_hex(0x3b4378),LV_STATE_DISABLED);
  lv_obj_set_style_bg_grad_color(button,lv_color_hex(0x262b56),LV_STATE_DISABLED);
  lv_obj_set_style_border_color(button,lv_color_hex(0x5d6694),LV_STATE_DISABLED);
  lv_obj_set_style_shadow_opa(button,LV_OPA_TRANSP,LV_STATE_DISABLED);
  auto caption=lv_label_create(button);lv_label_set_text(caption,"SMALL\nCUP");
  lv_obj_set_style_text_font(caption,&lv_font_montserrat_14,0);
  lv_obj_set_style_text_align(caption,LV_TEXT_ALIGN_CENTER,0);
  lv_obj_set_style_text_color(caption,lv_color_hex(0xffffff),0);
  lv_obj_center(caption);
  lv_obj_add_event_cb(button,action,LV_EVENT_CLICKED,nullptr);
  return button;
}
} // namespace pour_button
