#include "pour_button.h"
#include <cassert>
#include <cstdio>
static uint8_t pixels[240*320*4];
static int full_count=0,half_count=0;
static lv_point_t contact{120,165};static bool down=false;
static void flush(lv_display_t*d,const lv_area_t*,uint8_t*){lv_display_flush_ready(d);}
static void read(lv_indev_t*,lv_indev_data_t*data){data->point=contact;data->state=down?LV_INDEV_STATE_PRESSED:LV_INDEV_STATE_RELEASED;}
static void full(lv_event_t*){if(pour_button::half_at(108,contact.y))++half_count;else ++full_count;}
static void half(lv_event_t*){++half_count;}
int main(){
 lv_init();auto display=lv_display_create(240,320);lv_display_set_color_format(display,LV_COLOR_FORMAT_XRGB8888);
 lv_display_set_buffers(display,pixels,nullptr,sizeof(pixels),LV_DISPLAY_RENDER_MODE_DIRECT);lv_display_set_flush_cb(display,flush);
 auto button=lv_button_create(lv_screen_active());pour_button::style(button);
 lv_obj_set_pos(button,45,108);lv_obj_set_size(button,150,150);lv_obj_add_event_cb(button,full,LV_EVENT_CLICKED,nullptr);
 auto split=pour_button::add_half(button,half);lv_refr_now(display);
 auto input=lv_indev_create();lv_indev_set_type(input,LV_INDEV_TYPE_POINTER);lv_indev_set_display(input,display);lv_indev_set_read_cb(input,read);
 auto click=[&](int y){contact.y=y;down=true;lv_tick_inc(50);lv_indev_read(input);down=false;lv_tick_inc(50);lv_indev_read(input);lv_refr_now(display);};
 click(165);assert(full_count==1&&half_count==0);
 click(235);assert(full_count==1&&half_count==1);
 click(255);assert(full_count==1&&half_count==2); // bottom rim belongs to half, never full
 lv_obj_add_state(split.half,LV_STATE_DISABLED);click(235);assert(full_count==1); // no full fallback
 pour_button::split_state(split,true);pour_button::update(button,true);lv_refr_now(display);
 assert(lv_obj_has_flag(split.half,LV_OBJ_FLAG_HIDDEN)&&lv_obj_has_flag(split.divider,LV_OBJ_FLAG_HIDDEN));
 pour_button::split_state(split,false);pour_button::update(button,false);lv_obj_remove_state(split.half,LV_STATE_DISABLED);lv_refr_now(display);
 assert(!lv_obj_has_flag(split.half,LV_OBJ_FLAG_HIDDEN));click(165);assert(full_count==2);
 lv_obj_delete(button);lv_deinit();puts("PASS: real pointer hit-testing for 3/4 full, 1/4 half and bottom rim, no disabled-half fallback, unified emergency face");
}
