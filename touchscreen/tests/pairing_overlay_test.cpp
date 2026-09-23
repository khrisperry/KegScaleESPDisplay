#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
struct lv_obj_t {};
struct lv_timer_t {};
struct Action { char kind[24]; char body[1024]; };
constexpr int pdTRUE=1,LV_STATE_DISABLED=1;
static int actions;
static lv_obj_t node;
static lv_timer_t timer;
static lv_obj_t *pairing_overlay=&node,*cancel_button=&node,*cancel_label=&node;
static lv_timer_t *pairing_timeout_timer=&timer;
static int64_t pairing_deadline_us,clock_us;
static bool cancel_queued,queue_full;
static unsigned sends,deleted,cleaned;
static std::string label;
static int64_t esp_timer_get_time(){return clock_us;}
static bool bsp_display_lock(unsigned){return true;}
static void bsp_display_unlock(){}
static void lv_timer_delete(lv_timer_t *){}
static void lv_obj_delete(lv_obj_t *){deleted++;}
static void lv_obj_add_state(lv_obj_t *,int){}
static void lv_label_set_text(lv_obj_t *,const char *text){label=text;}
static void touchscreen_pairing_timeout_cleanup(){cleaned++;}
static int xQueueSend(int,const Action *action,int){
  assert(!strcmp(action->kind,"cancel_pairing"));
  if(queue_full)return 0;
  sends++;return pdTRUE;
}
#include "pairing_overlay.inc"
int main(){
  clock_us=10000000;
  touchscreen_pairing_window(73);
  pairing_tick(nullptr);assert(label=="Cancel pairing (1:13)");
  clock_us+=12500000;pairing_tick(nullptr);assert(label=="Cancel pairing (1:01)");
  clock_us=pairing_deadline_us;queue_full=true;pairing_tick(nullptr);
  assert(!cancel_queued&&!sends);
  queue_full=false;pairing_tick(nullptr);pairing_tick(nullptr);
  assert(cancel_queued&&sends==1&&label=="Pairing window ended");
  touchscreen_pairing_ended();
  assert(!pairing_overlay&&!cancel_label&&!pairing_timeout_timer&&deleted==1&&cleaned==1);
  pairing_overlay=cancel_button=cancel_label=&node;
  request_pairing_cancel(false);assert(cancel_queued&&sends==2&&label=="Canceling pairing...");
  touchscreen_pairing_ended();assert(deleted==2&&cleaned==2);
  touchscreen_pairing_ended();assert(deleted==2&&cleaned==2);
  puts("PASS: production pairing countdown, expiry, queued cancellation, queue-full retry, disconnect cleanup");
}
