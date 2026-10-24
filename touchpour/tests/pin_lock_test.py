"""Exercise the real PIN service with simulated NVS, clock, RNG and SHA adapter."""
from pathlib import Path
import subprocess
import tempfile
root=Path(__file__).resolve().parents[1]
source=(root/'components/tap_control/pin_lock.cpp').read_text()
source='\n'.join(line for line in source.splitlines() if not line.startswith('#include'))
harness=r'''
#include "pin_lock.h"
#include <atomic>
#include <cstring>
#include <cstdint>
#include <vector>
#include <cassert>
#include <cstdio>
using esp_err_t=int;using nvs_handle_t=int;
constexpr int ESP_OK=0,ESP_ERR_NVS_NOT_FOUND=1,NVS_READONLY=0,NVS_READWRITE=1;
bool commit_fails=false,read_fails=false,pending_erase=false;
std::vector<uint8_t> stored,pending;
uint32_t clock_ms=0;unsigned random_seed=0;
int64_t esp_timer_get_time(){return int64_t(clock_ms)*1000;}
void esp_fill_random(void*out,size_t size){auto p=static_cast<uint8_t*>(out);while(size--)*p++=uint8_t(++random_seed);}
// Hashing primitive is supplied by mbedTLS on-device; verify its adapter inputs.
int mbedtls_sha256(const uint8_t*data,size_t size,uint8_t*out,int is224){
 assert(size==20&&is224==0);uint32_t hash=2166136261u;
 for(size_t i=0;i<size;++i)hash=(hash^data[i])*16777619u;
 for(int i=0;i<32;++i){hash=hash*1664525u+1013904223u;out[i]=uint8_t(hash>>24);}return 0;
}
int nvs_open(const char*name,int mode,int*handle){assert(!strcmp(name,"tap_pin"));*handle=1;if(read_fails)return 2;if(mode==NVS_READONLY&&stored.empty())return ESP_ERR_NVS_NOT_FOUND;return ESP_OK;}
int nvs_get_blob(int,const char*,void*out,size_t*size){if(stored.size()>*size)return 2;*size=stored.size();memcpy(out,stored.data(),*size);return ESP_OK;}
int nvs_set_blob(int,const char*,const void*data,size_t size){auto b=static_cast<const uint8_t*>(data);pending.assign(b,b+size);pending_erase=false;return ESP_OK;}
int nvs_erase_key(int,const char*){pending_erase=true;return ESP_OK;}
int nvs_commit(int){if(commit_fails)return 2;if(pending_erase)stored.clear();else stored=pending;return ESP_OK;}
void nvs_close(int){}
#define ESP_LOGE(...) ((void)0)
'''+source+r'''
using namespace tap::pin_lock;
int main(){
 init();assert(!configured()&&!locked()&&!lock());
 assert(change("","12a4")==Result::InvalidPin&&!configured());
 assert(change("","0123")==Result::Ok&&configured()&&locked());
 assert(stored.size()==52);auto original=stored;
 assert(unlock("0123")==Result::Ok&&!locked());
 init();assert(configured()&&locked()); // power cycling cannot unlock
 for(int i=0;i<4;++i)assert(unlock("9999")==Result::WrongPin&&locked());
 assert(unlock("9999")==Result::Wait&&retry_seconds()==30);
 assert(unlock("0123")==Result::Wait&&locked());clock_ms+=29999;
 assert(retry_seconds()==1&&unlock("0123")==Result::Wait);
 ++clock_ms;assert(unlock("0123")==Result::Ok&&!locked());
 assert(change("9999","4567")==Result::WrongPin&&stored==original);
 commit_fails=true;assert(change("0123","4567")==Result::StorageError&&stored==original);
 commit_fails=false;assert(change("0123","4567")==Result::Ok&&locked()&&stored!=original);
 assert(unlock("0123")==Result::WrongPin);assert(unlock("4567")==Result::Ok);
 assert(tap::pin_lock::remove("0123")==Result::WrongPin&&configured());
 assert(tap::pin_lock::remove("4567")==Result::Ok&&!configured()&&!locked()&&stored.empty());
 init();assert(!locked()&&!configured());
 assert(change("","0000")==Result::Ok);clock_ms=UINT32_MAX-99;
 for(int i=0;i<5;++i){unlock("1111");}assert(locked());clock_ms+=30000;
 assert(unlock("0000")==Result::Ok); // cooldown survives clock wrap
 stored[0]=99;init();assert(configured()&&locked());
 assert(unlock("0000")==Result::StorageError); // damaged storage is fail-closed
 stored.clear();read_fails=true;init();assert(locked());
 assert(unlock("0000")==Result::StorageError);
 puts("PASS: PIN format/leading zeros, confirm service, reboot lock, rate limit, change/remove authorization, storage failure, clock wrap and fail-closed reads");
}
'''
with tempfile.TemporaryDirectory() as directory:
    path=Path(directory);(path/'test.cpp').write_text(harness)
    subprocess.run(['g++','-std=c++17','-Wall','-Wextra','-Werror','-I',str(root/'components/tap_control'),str(path/'test.cpp'),'-o',str(path/'test')],check=True)
    subprocess.run([str(path/'test')],check=True)
