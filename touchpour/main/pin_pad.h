#pragma once
#include "lvgl.h"
#include <cstring>

// Full-screen numeric PIN pad. V0.1.23 restyle: lock badge, four entry dots
// (LV_STATE_CHECKED = entered) and outlined keys. Key positions, the
// four-digit auto-submit and the no-escape locked mode are unchanged.
namespace pin_pad {
struct View {
  lv_obj_t *root=nullptr,*heading=nullptr,*dots=nullptr,*message=nullptr;
  char entered[5]{};
  unsigned length=0;
  void (*submit)(const char *)=nullptr;
  void (*cancel)()=nullptr;
};
inline bool dot_filled(const View &view,unsigned index){
  return lv_obj_has_state(lv_obj_get_child(view.dots,index),LV_STATE_CHECKED);
}
inline void show_length(View &view){
  for(unsigned i=0;i<4;++i){
    auto dot=lv_obj_get_child(view.dots,i);
    if(i<view.length)lv_obj_add_state(dot,LV_STATE_CHECKED);
    else lv_obj_remove_state(dot,LV_STATE_CHECKED);
  }
}
inline void clear(View &view){
  std::memset(view.entered,0,sizeof(view.entered));view.length=0;
  show_length(view);
}
inline void key(lv_event_t *event){
  auto &view=*static_cast<View *>(lv_event_get_user_data(event));
  auto object=static_cast<lv_obj_t *>(lv_event_get_target(event));
  const char *text=lv_label_get_text(lv_obj_get_child(object,0));
  if(std::strcmp(text,LV_SYMBOL_CLOSE)==0){if(view.cancel)view.cancel();return;}
  if(std::strcmp(text,LV_SYMBOL_BACKSPACE)==0){
    if(view.length)view.entered[--view.length]='\0';
  }else if(text[0]>='0'&&text[0]<='9'&&view.length<4){
    view.entered[view.length++]=text[0];
  }
  show_length(view);
  if(view.length==4){
    char pin[5];std::memcpy(pin,view.entered,sizeof(pin));
    clear(view);view.submit(pin);std::memset(pin,0,sizeof(pin));
  }
}
inline lv_obj_t *text(lv_obj_t *parent,const char *value,int y,const lv_font_t *font,uint32_t color=0xf2f6f8){
  auto object=lv_label_create(parent);lv_label_set_text(object,value);
  lv_obj_set_pos(object,8,y);lv_obj_set_size(object,224,24);
  lv_obj_set_style_text_color(object,lv_color_hex(color),0);
  lv_obj_set_style_text_font(object,font,0);
  lv_obj_set_style_text_align(object,LV_TEXT_ALIGN_CENTER,0);
  lv_label_set_long_mode(object,LV_LABEL_LONG_DOT);return object;
}
// Padlock drawn from two plain rectangles: no icon font or image needed.
inline void lock_badge(lv_obj_t *parent){
  auto badge=lv_obj_create(parent);lv_obj_remove_style_all(badge);
  lv_obj_set_pos(badge,104,10);lv_obj_set_size(badge,32,32);
  lv_obj_set_style_radius(badge,LV_RADIUS_CIRCLE,0);
  lv_obj_set_style_bg_opa(badge,LV_OPA_COVER,0);
  lv_obj_set_style_bg_color(badge,lv_color_hex(0x1e3240),0);
  lv_obj_set_style_border_width(badge,1,0);
  lv_obj_set_style_border_color(badge,lv_color_hex(0x2a4252),0);
  lv_obj_remove_flag(badge,LV_OBJ_FLAG_CLICKABLE);lv_obj_remove_flag(badge,LV_OBJ_FLAG_SCROLLABLE);
  auto shackle=lv_obj_create(badge);lv_obj_remove_style_all(shackle);
  lv_obj_set_pos(shackle,10,6);lv_obj_set_size(shackle,10,12);
  lv_obj_set_style_radius(shackle,5,0);
  lv_obj_set_style_border_width(shackle,2,0);
  lv_obj_set_style_border_color(shackle,lv_color_hex(0x7fd0ff),0);
  lv_obj_set_style_border_side(shackle,static_cast<lv_border_side_t>(LV_BORDER_SIDE_TOP|LV_BORDER_SIDE_LEFT|LV_BORDER_SIDE_RIGHT),0);
  auto body=lv_obj_create(badge);lv_obj_remove_style_all(body);
  lv_obj_set_pos(body,8,13);lv_obj_set_size(body,14,10);
  lv_obj_set_style_radius(body,2,0);
  lv_obj_set_style_bg_opa(body,LV_OPA_COVER,0);
  lv_obj_set_style_bg_color(body,lv_color_hex(0x7fd0ff),0);
}
inline void create(View &view,lv_obj_t *parent,const char *heading,
                   void (*submit)(const char *),void (*cancel)()){
  if(view.root)lv_obj_delete(view.root);
  view={};view.submit=submit;view.cancel=cancel;
  view.root=lv_obj_create(parent);lv_obj_remove_style_all(view.root);
  lv_obj_set_pos(view.root,0,0);lv_obj_set_size(view.root,240,320);
  lv_obj_remove_flag(view.root,LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_style_bg_color(view.root,lv_color_hex(0x0d1821),0);
  lv_obj_set_style_bg_opa(view.root,LV_OPA_COVER,0);
  lock_badge(view.root);
  view.heading=text(view.root,heading,48,&lv_font_montserrat_18);
  view.dots=lv_obj_create(view.root);lv_obj_remove_style_all(view.dots);
  lv_obj_set_pos(view.dots,76,80);lv_obj_set_size(view.dots,88,14);
  lv_obj_remove_flag(view.dots,LV_OBJ_FLAG_CLICKABLE);lv_obj_remove_flag(view.dots,LV_OBJ_FLAG_SCROLLABLE);
  for(int i=0;i<4;++i){
    auto dot=lv_obj_create(view.dots);lv_obj_remove_style_all(dot);
    lv_obj_set_pos(dot,i*25,0);lv_obj_set_size(dot,14,14);
    lv_obj_set_style_radius(dot,LV_RADIUS_CIRCLE,0);
    lv_obj_set_style_border_width(dot,2,0);
    lv_obj_set_style_border_color(dot,lv_color_hex(0x4a6f84),0);
    lv_obj_set_style_bg_opa(dot,LV_OPA_COVER,LV_STATE_CHECKED);
    lv_obj_set_style_bg_color(dot,lv_color_hex(0x33b8ff),LV_STATE_CHECKED);
    lv_obj_set_style_border_color(dot,lv_color_hex(0x33b8ff),LV_STATE_CHECKED);
    lv_obj_remove_flag(dot,LV_OBJ_FLAG_CLICKABLE);
  }
  view.message=text(view.root,"4-digit PIN",102,&lv_font_montserrat_14,0x93a8b6);
  const char *keys[]={"1","2","3","4","5","6","7","8","9",LV_SYMBOL_CLOSE,"0",LV_SYMBOL_BACKSPACE};
  for(int i=0;i<12;++i){
    if(i==9&&!cancel)continue;
    const bool digit=i!=9&&i!=11;
    auto button=lv_button_create(view.root);
    lv_obj_remove_style_all(button);
    lv_obj_set_pos(button,14+(i%3)*72,128+(i/3)*47);lv_obj_set_size(button,68,44);
    lv_obj_set_style_radius(button,10,0);
    lv_obj_set_style_bg_opa(button,digit?LV_OPA_COVER:LV_OPA_TRANSP,0);
    lv_obj_set_style_bg_color(button,lv_color_hex(0x1e3240),0);
    lv_obj_set_style_border_width(button,digit?1:0,0);
    lv_obj_set_style_border_color(button,lv_color_hex(0x2a4252),0);
    lv_obj_set_style_bg_opa(button,LV_OPA_COVER,LV_STATE_PRESSED);
    lv_obj_set_style_bg_color(button,lv_color_hex(0x347fbb),LV_STATE_PRESSED);
    auto label=lv_label_create(button);lv_label_set_text(label,keys[i]);
    lv_obj_set_style_text_color(label,lv_color_hex(digit?0xf2f6f8:0xc8d5de),0);
    lv_obj_set_style_text_font(label,&lv_font_montserrat_20,0);lv_obj_center(label);
    lv_obj_add_event_cb(button,key,LV_EVENT_CLICKED,&view);
  }
}
inline void destroy(View &view){
  if(view.root)lv_obj_delete(view.root);
  std::memset(view.entered,0,sizeof(view.entered));view={};
}
} // namespace pin_pad
