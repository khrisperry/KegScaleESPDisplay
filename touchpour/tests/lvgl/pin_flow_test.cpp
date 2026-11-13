#include "pin_pad.h"
#include "pin_lock.h"
#include <cassert>
#include <cstring>
#include <cstdio>
static unsigned home_count=0,setup_count=0;
void show_home(){++home_count;}
void show_setup(){++setup_count;}
void message(const char*){}
namespace tap {
bool setup_mode=true;
bool busy(){return false;}
namespace pin_lock {
bool configured_value=false,locked_value=false;char saved_pin[5]{};
Result check(const char *pin){return configured_value&&!strcmp(saved_pin,pin)?Result::Ok:Result::WrongPin;}
Result unlock(const char *pin){auto result=check(pin);if(result==Result::Ok)locked_value=false;return result;}
Result change(const char *current,const char *pin){if(configured_value&&check(current)!=Result::Ok)return Result::WrongPin;memcpy(saved_pin,pin,5);configured_value=locked_value=true;return Result::Ok;}
Result remove(const char *pin){auto result=check(pin);if(result==Result::Ok)configured_value=locked_value=false;return result;}
}
}
#include "pin_flow.inc"
static uint8_t pixels[240*320*4];
static void flush(lv_display_t*d,const lv_area_t*,uint8_t*){lv_display_flush_ready(d);}
static void digits(const char *pin){
 for(unsigned n=0;n<4;++n){
  bool found=false;
  for(unsigned i=3;i<lv_obj_get_child_count(pin_view.root);++i){
   auto button=lv_obj_get_child(pin_view.root,i);
   if(lv_label_get_text(lv_obj_get_child(button,0))[0]==pin[n]){
    lv_obj_send_event(button,LV_EVENT_CLICKED,nullptr);found=true;break;
   }
  }
  assert(found);lv_tick_inc(50);lv_refr_now(lv_display_get_default());
 }
}
int main(){
 lv_init();auto display=lv_display_create(240,320);lv_display_set_color_format(display,LV_COLOR_FORMAT_XRGB8888);
 lv_display_set_buffers(display,pixels,nullptr,sizeof(pixels),LV_DISPLAY_RENDER_MODE_DIRECT);lv_display_set_flush_cb(display,flush);
 begin_pin(PinMode::New);digits("0123");assert(pin_mode==PinMode::Confirm);
 digits("0124");assert(pin_mode==PinMode::New&&!tap::pin_lock::configured_value);
 digits("0123");digits("0123");assert(pin_mode==PinMode::Unlock&&tap::pin_lock::locked_value&&!tap::setup_mode);
 assert(!current_pin[0]&&!new_pin[0]);digits("9999");assert(pin_view.root&&home_count==0);
 digits("0123");assert(!pin_view.root&&home_count==1&&!tap::pin_lock::locked_value);
 begin_pin(PinMode::VerifyChange);digits("9999");assert(pin_mode==PinMode::VerifyChange);
 digits("0123");assert(pin_mode==PinMode::New);digits("5678");digits("5678");
 assert(pin_mode==PinMode::Unlock&&tap::pin_lock::locked_value);digits("5678");assert(home_count==2);
 begin_pin(PinMode::VerifyChange);digits("5678");digits("1234");pin_cancel();
 assert(!current_pin[0]&&!new_pin[0]&&!pin_view.root&&setup_count==1);
 begin_pin(PinMode::Remove);digits("9999");assert(pin_view.root&&tap::pin_lock::configured_value);
 digits("5678");assert(!pin_view.root&&!tap::pin_lock::configured_value&&setup_count==2);
 lv_deinit();puts("PASS: real PIN screen flow: mismatch, set, wrong/correct unlock, current-PIN verification, change, cancel, secret clearing and removal");
}
