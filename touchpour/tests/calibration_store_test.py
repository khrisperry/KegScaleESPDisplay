"""Exercise the actual calibration save/load with the separate half-target key."""
from pathlib import Path
import subprocess,tempfile
root=Path(__file__).resolve().parents[1]
source=(root/'components/tap_control/app.cpp').read_text()
save=source[source.index('bool save_settings('):source.index('static void set_servo(')]
start=source[source.index('void start()'):]
load=start[start.index('  nvs_handle_t h;'):start.index('  active_stop_mm=settings.stop_mm;')]
harness=r'''
#include "pour_target.h"
#include <cassert>
#include <cstring>
#include <cstdio>
#include <vector>
using nvs_handle_t=int;using esp_err_t=int;
constexpr int ESP_OK=0,NVS_READWRITE=1,NVS_READONLY=0;
std::vector<unsigned char> disk,staged;bool have_half=false,have_small=false,fail_commit=false;int32_t disk_half=0,staged_half=0,disk_small=0,staged_small=0;
int nvs_open(const char*,int,int*h){*h=1;return 0;}
int nvs_set_blob(int,const char*,const void*d,size_t n){auto p=static_cast<const unsigned char*>(d);staged.assign(p,p+n);return 0;}
int nvs_set_i32(int,const char*k,int32_t v){if(!strcmp(k,"half_stop_mm"))staged_half=v;else{assert(!strcmp(k,"small_stop_mm"));staged_small=v;}return 0;}
int nvs_commit(int){if(fail_commit)return 1;disk=staged;disk_half=staged_half;disk_small=staged_small;have_half=have_small=true;return 0;}
int nvs_get_blob(int,const char*,void*d,size_t*n){if(disk.size()>*n)return 1;*n=disk.size();memcpy(d,disk.data(),*n);return 0;}
int nvs_get_i32(int,const char*k,int32_t*v){if(!strcmp(k,"small_stop_mm")){if(!have_small)return 1;*v=disk_small;return 0;}if(!have_half)return 1;*v=disk_half;return 0;}
void nvs_close(int){}
namespace tap{
Settings settings;int half_stop_mm=0,small_stop_mm=0;
bool save_settings(const Settings&,int half_target,int small_target=-1); // header default
'''+save+'''
void reload(){settings={};half_stop_mm=0;small_stop_mm=0;
'''+load+r'''}
}
using namespace tap;
int main(){
 Settings original;original.stop_mm=80;original.min_mm=237;original.max_mm=267;original.tray_mm=299;
 original.servo_calibrated=original.distance_calibrated=true;
 disk.resize(sizeof(original));memcpy(disk.data(),&original,sizeof(original));
 reload();assert(settings.stop_mm==80&&settings.closed_us==original.closed_us&&half_stop_mm==0);
 assert(save_settings(settings,160));auto old=disk;
 reload();assert(half_stop_mm==160&&settings.stop_mm==80&&disk==old);
 assert(!save_settings(settings,80)&&!save_settings(settings,300));
 fail_commit=true;assert(!save_settings(settings,170));fail_commit=false;
 reload();assert(half_stop_mm==160&&settings.stop_mm==80);
 assert(save_settings(settings,0));reload();assert(half_stop_mm==0);
 disk_half=999;reload();assert(half_stop_mm==0&&settings.stop_mm==80); // bad half key never discards full calibration
 have_small=false;reload();assert(small_stop_mm==0); // V0.1.22 storage has no small-cup key: stays disabled
 assert(save_settings(settings,160,180));reload();assert(half_stop_mm==160&&small_stop_mm==180&&settings.stop_mm==80);
 assert(save_settings(settings,-1,-1));reload();assert(half_stop_mm==160&&small_stop_mm==180); // -1 keeps both
 assert(!save_settings(settings,160,80)&&!save_settings(settings,160,300));reload();assert(small_stop_mm==180);
 assert(save_settings(settings,0,0));reload();assert(half_stop_mm==0&&small_stop_mm==0);
 disk_small=999;reload();assert(small_stop_mm==0&&settings.stop_mm==80); // bad small key never discards full calibration
 puts("PASS: independent small-cup save/reboot/disable/upgrade; legacy calibration retained, independent half-target save/reboot/disable, invalid target and failed commit");
}
'''
with tempfile.TemporaryDirectory() as directory:
 path=Path(directory);(path/'test.cpp').write_text(harness)
 subprocess.run(['g++','-std=c++17','-Wall','-Wextra','-Werror','-I',str(root/'components/tap_control'),str(path/'test.cpp'),'-o',str(path/'test')],check=True)
 subprocess.run([str(path/'test')],check=True)
