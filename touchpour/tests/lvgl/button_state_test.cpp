#include "pour_button.h"
#include <cassert>
#include <cstdio>

static uint8_t pixels[240 * 320 * 4];
static void flush(lv_display_t *display,const lv_area_t *,uint8_t *) {
  lv_display_flush_ready(display);
}
int main() {
  lv_init();
  auto display=lv_display_create(240,320);
  lv_display_set_color_format(display,LV_COLOR_FORMAT_XRGB8888);
  lv_display_set_buffers(display,pixels,nullptr,sizeof(pixels),LV_DISPLAY_RENDER_MODE_DIRECT);
  lv_display_set_flush_cb(display,flush);
  auto button=lv_button_create(lv_screen_active());
  lv_obj_set_pos(button,45,108);lv_obj_set_size(button,150,150);
  pour_button::style(button);
  auto redraw=[&] {lv_tick_inc(250);lv_refr_now(display);};
  // LVGL skips transitions until an object has actually been rendered.
  // Changing state only before the initial draw misses the null-dereference bug.
  redraw();
  for(int cycle=0;cycle<20;++cycle) {
    lv_obj_add_state(button,LV_STATE_DISABLED);redraw();
    assert(lv_obj_has_state(button,LV_STATE_DISABLED));
    lv_obj_remove_state(button,LV_STATE_DISABLED);redraw();
    lv_obj_add_state(button,LV_STATE_PRESSED);redraw();
    pour_button::update(button,true);redraw();
    assert(lv_obj_has_state(button,LV_STATE_USER_1));
    lv_obj_remove_state(button,LV_STATE_PRESSED);redraw();
    pour_button::update(button,false);redraw();
    assert(!lv_obj_has_state(button,LV_STATE_USER_1));
  }
  lv_obj_delete(button);lv_deinit();
  puts("PASS: actual LVGL rendered button survives disabled, enabled, pressed, released and pouring transitions");
}
