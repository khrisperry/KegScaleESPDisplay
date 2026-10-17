"""Run the actual UI poll with counted display mutations (no ESP32 needed)."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
source = (root / 'main/ui.cpp').read_text()
helpers = source[source.index('void text_if_changed('):source.index('bool submit(')]
poll = source[source.index('void poll(lv_timer_t *)'):source.index('\n}\nvoid ui_start')]
harness = r'''
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
int mutations=0, styles=0;
struct lv_obj_t {std::string text;int state=0,flags=0;};
struct lv_timer_t {};
constexpr int LV_STATE_DISABLED=1,LV_ALIGN_CENTER=0,LV_OBJ_FLAG_HIDDEN=1,pdTRUE=1;
int lv_font_montserrat_20,lv_font_montserrat_28;
const char *lv_label_get_text(lv_obj_t*o){return o->text.c_str();}
void lv_label_set_text(lv_obj_t*o,const char*s){o->text=s;++mutations;}
void lv_obj_set_style_text_font(lv_obj_t*,int*,int){++styles;}
void lv_obj_align(lv_obj_t*,int,int,int){++styles;}
void lv_obj_add_flag(lv_obj_t*o,int f){if(!(o->flags&f))++mutations;o->flags|=f;}
void lv_obj_remove_flag(lv_obj_t*o,int f){if(o->flags&f)++mutations;o->flags&=~f;}
void lv_obj_add_state(lv_obj_t*o,int f){if(!(o->state&f))++mutations;o->state|=f;}
void lv_obj_remove_state(lv_obj_t*o,int f){if(o->state&f)++mutations;o->state&=~f;}
namespace pour_button {void update(lv_obj_t*,bool){++styles;}}
namespace tap {
struct Settings{int stale_ms=350;}settings;
struct Sample{bool valid=true,filtered_valid=true;uint32_t at=1000;int mm=229;int stable_distance()const{return mm;}}sample;
bool active=false,ready=true,manual_test_active=false,setup_mode=false,firmware_updating=false,history_saving=false;
struct Control{const char*reason="Ready";bool ready(const Settings&,int)const{return tap::ready;}}control;
int state_mutex=1;
bool pouring_active(){return active;}
const char*idle_status(const Settings&,const Sample&,uint32_t,bool ready,const char*){return ready?"Cup detected - Ready":"No cup detected";}
}
int xSemaphoreTake(int,int){return pdTRUE;}void xSemaphoreGive(int){}
int64_t esp_timer_get_time(){return 1000000;}
lv_obj_t btn,text,caption,foot,status;
lv_obj_t *auto_btn=&btn,*auto_text=&text,*auto_caption=&caption,*footer=&foot,*notice=&status;
int page=0;bool pairing=false,pour_state_known=false,displayed_pouring=false;
struct {bool valid=true,online=true;}reading;
'''+helpers+poll+r'''
void stable(){mutations=styles=0;for(int i=0;i<1000;++i)poll(nullptr);assert(mutations==0&&styles==0);}
int main(){
 poll(nullptr);assert(text.text=="POUR"&&foot.text=="LiDAR: 229 mm");stable();
 tap::active=true;poll(nullptr);assert(text.text=="EMERGENCY\nSTOP"&&status.text=="Touch anywhere to stop");stable();
 tap::sample.mm=230;mutations=styles=0;poll(nullptr);assert(mutations==1&&styles==0&&foot.text=="LiDAR: 230 mm");
 tap::active=false;tap::ready=false;poll(nullptr);assert(text.text=="POUR"&&(btn.state&LV_STATE_DISABLED));stable();
 tap::manual_test_active=true;poll(nullptr);assert(text.text=="EMERGENCY\nSTOP"&&!(btn.state&LV_STATE_DISABLED));stable();
 puts("PASS: actual UI poll has no unchanged text/font/alignment redraws at idle or pouring; LiDAR-only updates, disabled and manual-test transitions");
}
'''
with tempfile.TemporaryDirectory() as directory:
    path = Path(directory)
    (path / 'test.cpp').write_text(harness)
    subprocess.run(['g++', '-std=c++17', '-Wall', '-Wextra', '-Werror', str(path/'test.cpp'), '-o', str(path/'test')],check=True)
    subprocess.run([str(path/'test')],check=True)
