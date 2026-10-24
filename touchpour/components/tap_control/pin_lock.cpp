#include "pin_lock.h"
#include "nvs.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "mbedtls/sha256.h"
#include <atomic>
#include <cstring>

namespace tap::pin_lock {
namespace {
struct Record {uint32_t version=1;uint8_t salt[16]{};uint8_t digest[32]{};};
Record saved;
std::atomic<bool> enabled{false},closed{false};
bool storage_ok=true,cooling=false;
unsigned failures=0;
uint32_t failed_at=0;
constexpr uint32_t cooldown_ms=30000;
uint32_t now(){return uint32_t(esp_timer_get_time()/1000);}
bool valid(const char *pin){
  if(!pin)return false;
  for(unsigned i=0;i<4;++i)if(pin[i]<'0'||pin[i]>'9')return false;
  return pin[4]=='\0';
}
bool digest(const Record &record,const char *pin,uint8_t *out){
  uint8_t input[20];std::memcpy(input,record.salt,16);std::memcpy(input+16,pin,4);
  const int result=mbedtls_sha256(input,sizeof(input),out,0);
  std::memset(input,0,sizeof(input));return result==0;
}
bool persist(const Record *record){
  nvs_handle_t handle;
  if(nvs_open("tap_pin",NVS_READWRITE,&handle)!=ESP_OK)return false;
  esp_err_t result=record?nvs_set_blob(handle,"record",record,sizeof(*record)):nvs_erase_key(handle,"record");
  if(result==ESP_OK)result=nvs_commit(handle);
  nvs_close(handle);return result==ESP_OK;
}
}
void init(){
  saved={};failures=0;cooling=false;storage_ok=true;enabled=false;closed=false;
  nvs_handle_t handle;
  esp_err_t result=nvs_open("tap_pin",NVS_READONLY,&handle);
  if(result==ESP_ERR_NVS_NOT_FOUND)return;
  if(result==ESP_OK){
    size_t size=sizeof(saved);result=nvs_get_blob(handle,"record",&saved,&size);nvs_close(handle);
    if(result==ESP_ERR_NVS_NOT_FOUND)return;
    if(result==ESP_OK&&size==sizeof(saved)&&saved.version==1){enabled=true;closed=true;return;}
  }
  // A damaged/read-failing configured store must not silently unlock the tap.
  storage_ok=false;enabled=true;closed=true;
  ESP_LOGE("tap_pin","PIN storage unavailable; physical tap remains locked");
}
bool configured(){return enabled.load();}
bool locked(){return closed.load();}
bool lock(){if(!enabled)return false;closed=true;return true;}
uint32_t retry_seconds(){
  if(!cooling)return 0;
  uint32_t elapsed=now()-failed_at;
  if(elapsed>=cooldown_ms){cooling=false;failures=0;return 0;}
  return (cooldown_ms-elapsed+999)/1000;
}
Result check(const char *pin){
  if(!storage_ok)return Result::StorageError;
  if(retry_seconds())return Result::Wait;
  uint8_t actual[32]{};
  bool matches=enabled&&valid(pin)&&digest(saved,pin,actual);
  unsigned difference=0;
  for(unsigned i=0;i<sizeof(actual);++i)difference|=actual[i]^saved.digest[i];
  std::memset(actual,0,sizeof(actual));matches=matches&&difference==0;
  if(matches){failures=0;cooling=false;return Result::Ok;}
  if(++failures>=5){failed_at=now();cooling=true;return Result::Wait;}
  return Result::WrongPin;
}
Result unlock(const char *pin){
  Result result=check(pin);if(result==Result::Ok)closed=false;return result;
}
Result change(const char *current,const char *pin){
  if(!storage_ok)return Result::StorageError;
  if(!valid(pin))return Result::InvalidPin;
  if(enabled){Result result=check(current);if(result!=Result::Ok)return result;}
  Record next;esp_fill_random(next.salt,sizeof(next.salt));
  if(!digest(next,pin,next.digest)||!persist(&next))return Result::StorageError;
  saved=next;failures=0;cooling=false;enabled=true;closed=true;
  return Result::Ok;
}
Result remove(const char *current){
  Result result=check(current);if(result!=Result::Ok)return result;
  if(!persist(nullptr))return Result::StorageError;
  saved={};enabled=false;closed=false;return Result::Ok;
}
} // namespace tap::pin_lock
