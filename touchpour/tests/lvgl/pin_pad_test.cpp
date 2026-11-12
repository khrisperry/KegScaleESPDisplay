#include "pin_pad.h"
#include <cassert>
#include <cstdio>
#include <cstring>
static uint8_t pixels[240*320*4];
static pin_pad::View view;
static char received[5]{};static unsigned submitted=0,cancelled=0;
static void flush(lv_display_t *d,const lv_area_t *,uint8_t *){lv_display_flush_ready(d);}
static void submit(const char *pin){std::memcpy(received,pin,5);++submitted;pin_pad::destroy(view);}
static void cancel(){++cancelled;pin_pad::destroy(view);}
static lv_obj_t *find(const char *key){
  for(unsigned i=3;i<lv_obj_get_child_count(view.root);++i){
    auto button=lv_obj_get_child(view.root,i);
    if(!std::strcmp(lv_label_get_text(lv_obj_get_child(button,0)),key))return button;
  }return nullptr;
}
static void press(const char *key){auto button=find(key);assert(button);lv_obj_send_event(button,LV_EVENT_CLICKED,nullptr);}
int main(){
 lv_init();auto display=lv_display_create(240,320);lv_display_set_color_format(display,LV_COLOR_FORMAT_XRGB8888);
 lv_display_set_buffers(display,pixels,nullptr,sizeof(pixels),LV_DISPLAY_RENDER_MODE_DIRECT);lv_display_set_flush_cb(display,flush);
 pin_pad::create(view,lv_screen_active(),"Enter PIN to unlock",submit,nullptr);lv_refr_now(display);
 assert(!find(LV_SYMBOL_CLOSE)); // locked screen has no cancel/settings escape
 press("0");press("1");assert(!std::strcmp(lv_label_get_text(view.dots),"* * _ _"));
 press(LV_SYMBOL_BACKSPACE);press("2");press("3");press("4");
 assert(submitted==1&&!std::strcmp(received,"0234")&&!view.root); // leading zero, safe deletion during event
 for(int i=0;i<10;++i){
  pin_pad::create(view,lv_screen_active(),"Confirm new PIN",submit,cancel);lv_tick_inc(100);lv_refr_now(display);
  press("1");press(LV_SYMBOL_CLOSE);assert(!view.root);
 }
 assert(cancelled==10);lv_deinit();puts("PASS: actual LVGL numeric-only pad, masked entry, backspace, leading zero, no locked escape and event-time page deletion");
}
