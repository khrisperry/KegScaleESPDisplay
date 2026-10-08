"""Exercise the actual controller loop with timed GPIO/sensor inputs."""
import pathlib
import subprocess
import tempfile
root=pathlib.Path(__file__).resolve().parents[1]
source=(root/'components/tap_control/app.cpp').read_text()
controller=source[source.index('static void controller_task('):source.index('static void measurement_log_task(')]
harness=r'''
#include "button_gesture.h"
using namespace tap;
#include <atomic>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <vector>
Settings settings;
Sample sample;
Control control,manual_hold;
bool pouring_active(){return control.pouring||manual_hold.pouring;}
std::atomic<bool> stop_requested{false},setup_mode{false},history_saving{false},touch_stop_armed{false};
bool manual_test_active=false,firmware_updating=false,update_closed=false;
int servo_us=1000,state_mutex=1,actions=1;
constexpr int portMAX_DELAY=0,pdTRUE=1,CONFIG_TAP_BUTTON=7;
const char *TAG="tap";
struct Action{enum Kind{START,CLOSE,TEST}kind;int pulse;};
using gpio_num_t=int;
uint32_t clock_ms=0,limit=0;
int scenario=0,opens=0,closes=0;
uint32_t opened=0,closed=0;
uint32_t now_ms(){return clock_ms;}
int gpio_get_level(int){
  if(scenario>=10)return !((clock_ms>=1000&&clock_ms<1100)||(scenario==11&&clock_ms>=2500&&clock_ms<2600));
  if(scenario==0)return !(clock_ms>=1000&&clock_ms<1100);
  if(scenario==4)return !((clock_ms<3000)||(clock_ms>=3500&&clock_ms<6000));
  return !(clock_ms>=1000&&clock_ms<4500);
}
void xSemaphoreTake(int,int){}
void xSemaphoreGive(int){}
void xQueueReset(int){}
int xQueueReceive(int,Action*,int){return 0;}
void set_servo(int us){if(us==servo_us)return;servo_us=us;if(us==2500){++opens;opened=clock_ms;}else{++closes;closed=clock_ms;}}
void foam_observe(const Sample&,const Settings&,uint32_t,int,bool,bool,bool,bool,int,const char*,bool){}
void history_observe(const Sample&,const Settings&,uint32_t,int,bool,bool,bool,const char*){}
template<class... T> void log(const char*,const char*,T...){}
#define ESP_LOGI(...) log(__VA_ARGS__)
#define pdMS_TO_TICKS(x) (x)
struct Done{};
void vTaskDelay(int ms){
  clock_ms+=ms;
  if(clock_ms>=limit)throw Done{};
  if(clock_ms%100==0 && !(scenario==8&&clock_ms>500)){int mm=scenario>=10?200:scenario==0?(clock_ms>=1800?75:200):scenario==7?(clock_ms%200==0?20:1900):60;bool valid=!(scenario==2&&clock_ms>=3300)&&scenario!=6;sample={mm,valid?11:-1,valid,clock_ms,clock_ms/100+1};}
  if(scenario==3&&clock_ms==3200)stop_requested=true;
  if(scenario>=10&&clock_ms==1500){assert(touch_stop_armed);stop_requested=true;}
}
'''+controller+r'''
void run(int which,uint32_t until){
  scenario=which;clock_ms=0;limit=until;opens=closes=0;opened=closed=0;
  settings={};settings.servo_calibrated=settings.distance_calibrated=true;settings.closed_us=1000;settings.open_us=2500;
  control={};manual_hold={};sample={};servo_us=1000;stop_requested=false;
  if(which==5)settings.max_pour_ms=1000;
  if(which==6)settings.distance_calibrated=false;
  if(which==9)settings.servo_calibrated=false;
  try{controller_task(nullptr);}catch(Done&){}
}
int main(){
  run(0,2500);assert(opens==1&&opened==1130&&closed==1800&&!pouring_active());assert(!strcmp(control.reason,"Fill target reached"));
  run(1,5000);assert(opens==1&&opened==3030&&closed==4500&&!pouring_active());assert(!strcmp(control.reason,"Manual fill: button released"));
  run(2,5000);assert(opens==1&&closed==4500&&!pouring_active());
  run(3,5000);assert(opens==1&&closed==3200&&!pouring_active());assert(!strcmp(control.reason,"Emergency stop"));
  run(4,6500);assert(opens==1&&opened==5530&&closed==6000&&!pouring_active());
  run(5,5000);assert(opens==1&&closed==4030&&!pouring_active());assert(!strcmp(control.reason,"Maximum pour time"));
  run(6,5000);assert(opens==1&&opened==3030&&closed==4500&&!pouring_active());
  run(7,5000);assert(opens==1&&closed==4500&&!pouring_active());
  run(8,5000);assert(opens==1&&closed==4500&&!pouring_active());
  run(9,5000);assert(opens==0&&!pouring_active());
  run(10,3500);assert(opens==1&&opened==1130&&closed==1500&&!pouring_active()&&!touch_stop_armed);
  run(11,3500);assert(opens==2&&opened==2630&&closed==1500&&pouring_active()&&touch_stop_armed);
  puts("PASS: emergency touch closes actual controller, clears armed state, allows a fresh later press;  real controller tap/cutoff, manual hold ignores invalid/stale/missing/cup/jump readings and distance calibration, immediate release, STOP lockout, held-at-boot, servo calibration and maximum time");
}
'''
with tempfile.TemporaryDirectory() as directory:
    d=pathlib.Path(directory);(d/'test.cpp').write_text(harness)
    subprocess.run(['g++','-std=c++17','-Wall','-Wextra','-Werror','-I',str(root/'components/tap_control'),str(d/'test.cpp'),'-o',str(d/'test')],check=True)
    subprocess.run([str(d/'test')],check=True)
