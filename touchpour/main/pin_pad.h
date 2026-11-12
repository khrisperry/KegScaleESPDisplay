#pragma once
#include "lvgl.h"
#include <cstring>

namespace pin_pad {
struct View {
  lv_obj_t *root=nullptr,*heading=nullptr,*dots=nullptr,*message=nullptr;
  char entered[5]{};
  unsigned length=0;
  void (*submit)(const char *)=nullptr;
  void (*cancel)()=nullptr;
};
inline void clear(View &view){
  std::memset(view.entered,0,sizeof(view.entered));view.length=0;
  lv_label_set_text(view.dots,"_ _ _ _");
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
  char dots[]="_ _ _ _";
  for(unsigned i=0;i<view.length;++i)dots[2*i]='*';
  lv_label_set_text(view.dots,dots);
  if(view.length==4){
    char pin[5];std::memcpy(pin,view.entered,sizeof(pin));
    clear(view);view.submit(pin);std::memset(pin,0,sizeof(pin));
  }
}
inline lv_obj_t *text(lv_obj_t *parent,const char *value,int y,const lv_font_t *font){
  auto object=lv_label_create(parent);lv_label_set_text(object,value);
  lv_obj_set_pos(object,8,y);lv_obj_set_size(object,224,32);
  lv_obj_set_style_text_color(object,lv_color_hex(0xf2f6f8),0);
  lv_obj_set_style_text_font(object,font,0);
  lv_obj_set_style_text_align(object,LV_TEXT_ALIGN_CENTER,0);
  lv_label_set_long_mode(object,LV_LABEL_LONG_DOT);return object;
}
inline void create(View &view,lv_obj_t *parent,const char *heading,
                   void (*submit)(const char *),void (*cancel)()){
  if(view.root)lv_obj_delete(view.root);
  view={};view.submit=submit;view.cancel=cancel;
  view.root=lv_obj_create(parent);lv_obj_remove_style_all(view.root);
  lv_obj_set_pos(view.root,0,0);lv_obj_set_size(view.root,240,320);
  lv_obj_remove_flag(view.root,LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_style_bg_color(view.root,lv_color_hex(0x101c26),0);
  lv_obj_set_style_bg_opa(view.root,LV_OPA_COVER,0);
  view.heading=text(view.root,heading,14,&lv_font_montserrat_20);
  view.dots=text(view.root,"_ _ _ _",50,&lv_font_montserrat_28);
  view.message=text(view.root,"4-digit PIN",84,&lv_font_montserrat_14);
  const char *keys[]={"1","2","3","4","5","6","7","8","9",LV_SYMBOL_CLOSE,"0",LV_SYMBOL_BACKSPACE};
  for(int i=0;i<12;++i){
    if(i==9&&!cancel)continue;
    auto button=lv_button_create(view.root);
    lv_obj_remove_style_all(button);
    lv_obj_set_pos(button,14+(i%3)*72,112+(i/3)*49);lv_obj_set_size(button,68,44);
    lv_obj_set_style_radius(button,8,0);
    lv_obj_set_style_bg_opa(button,LV_OPA_COVER,0);
    lv_obj_set_style_bg_color(button,lv_color_hex(0x203f58),0);
    lv_obj_set_style_bg_color(button,lv_color_hex(0x347fbb),LV_STATE_PRESSED);
    auto label=lv_label_create(button);lv_label_set_text(label,keys[i]);
    lv_obj_set_style_text_color(label,lv_color_hex(0xf2f6f8),0);
    lv_obj_set_style_text_font(label,&lv_font_montserrat_20,0);lv_obj_center(label);
    lv_obj_add_event_cb(button,key,LV_EVENT_CLICKED,&view);
  }
}
inline void destroy(View &view){
  if(view.root)lv_obj_delete(view.root);
  std::memset(view.entered,0,sizeof(view.entered));view={};
}
} // namespace pin_pad
