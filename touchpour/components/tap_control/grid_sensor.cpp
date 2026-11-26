#include "grid_sensor.h"
#include "tap_app.h"
#include "distance_filter.h"
#include "VL53L0X.h"
#include "vl53l7cx_api.h"
#include "nvs.h"
#include "esp_timer.h"
#include "esp_log.h"
namespace tap {
TAP_EXT_BSS GridStore grid_store;
GridFrame grid_frame;
GridMatch grid_match;
bool grid_sensor_present=false;
int grid_stable_frames=0;
static TAP_EXT_BSS VL53L7CX_Configuration dev;
static TAP_EXT_BSS VL53L7CX_ResultsData results;
void grid_load(){
  nvs_handle_t h;if(nvs_open("tap",NVS_READONLY,&h)!=ESP_OK)return;
  static TAP_EXT_BSS GridStore loaded;size_t n=sizeof(loaded);
  if(nvs_get_blob(h,"grid_v1",&loaded,&n)==ESP_OK&&n==sizeof(loaded)&&valid_grid_store(loaded))grid_store=loaded;
  nvs_close(h);
}
bool grid_save(const GridStore &s){
  if(!valid_grid_store(s))return false;
  nvs_handle_t h;if(nvs_open("tap",NVS_READWRITE,&h)!=ESP_OK)return false;
  esp_err_t e=nvs_set_blob(h,"grid_v1",&s,sizeof(s));if(e==ESP_OK)e=nvs_commit(h);
  nvs_close(h);return e==ESP_OK;
}
bool grid_ready(uint32_t now,bool half){
  if(!grid_store.enabled)return true;
  if(!grid_sensor_present || !grid_match.recognized || grid_stable_frames<5 ||
     uint32_t(now-grid_frame.at)>uint32_t(settings.stale_ms))return false;
  const auto &p=grid_store.profiles[grid_match.profile];
  return p.full && (!half||p.half);
}
Settings grid_settings(const Settings &base){
  auto c=base;
  if(!grid_store.enabled && !grid_sensor_present)return c;
  c.distance_calibrated=false;
  if(!grid_match.recognized)return c;
  const auto &p=grid_store.profiles[grid_match.profile];
  c.stop_mm=grid_match.rim+p.full_gap;
  c.tray_mm=*std::max_element(grid_store.background,grid_store.background+64);
  c.max_mm=c.tray_mm-31;c.min_mm=c.stop_mm+1;
  c.distance_calibrated=p.full && c.stop_mm>=20 && c.stop_mm<c.max_mm;
  return c;
}
int grid_half_target(int legacy){
  if(!grid_store.enabled && !grid_sensor_present)return legacy;
  if(!grid_match.recognized)return 0;
  const auto &p=grid_store.profiles[grid_match.profile];
  return p.full&&p.half?grid_match.rim+p.half_gap:0;
}
int grid_small_target(int legacy){
  if(!grid_store.enabled && !grid_sensor_present)return legacy;
  return 0;
}
void grid_sensor_task(void *){
  bool bus_ok=Wire.begin(CONFIG_TAP_SDA,CONFIG_TAP_SCL);
  // The 0X wrapper owns this separate external-sensor bus; panel touch uses bus 1.
  dev.platform.handle=Wire.native_device();dev.platform.address=0x29;
  bool initialized=false,checked=false,use_grid=false;
  VL53L0X old;old.setTimeout(120);
  uint32_t sequence=0,last_frame=0;
  DistanceFilter filter;
  for(;;){
    Wire.healthy=true;
    if(!initialized){
      // Read silicon IDs without writing either sensor's configuration.
      uint8_t id[2]={};uint8_t reg[2]={0x7f,0xff},request[3]={0x7f,0xff,0};
      // VL53L7CX ID is 0xf0/0x02 on page 0. Never probe the 0X with a page write
      // unless its 8-bit model-ID register has first ruled it out.
      old.readReg(0xc0);bool is_old=Wire.healthy && old.readReg(0xc0)==0xee;
      if(bus_ok&&!is_old&&Wire.native_device()){
        if(i2c_master_transmit(Wire.native_device(),request,3,50)==ESP_OK){
          reg[0]=reg[1]=0;
          if(i2c_master_transmit_receive(Wire.native_device(),reg,2,id,2,50)==ESP_OK)use_grid=id[0]==0xf0&&id[1]==0x02;
        }
      }
      if(use_grid){
        initialized=vl53l7cx_init(&dev)==0 &&
          vl53l7cx_set_resolution(&dev,VL53L7CX_RESOLUTION_8X8)==0 &&
          vl53l7cx_set_ranging_frequency_hz(&dev,10)==0 &&
          vl53l7cx_set_target_order(&dev,VL53L7CX_TARGET_ORDER_CLOSEST)==0 &&
          vl53l7cx_start_ranging(&dev)==0;
      }else if(is_old){
        Wire.healthy=true;initialized=old.init()&&Wire.healthy;
        if(initialized){initialized=old.setMeasurementTimingBudget(50000);old.startContinuous(100);initialized&=Wire.healthy;}
      }
      if(!checked||initialized)ESP_LOGI("grid","Sensor=%s initialized=%d",use_grid?"VL53L7CX 8x8":is_old?"VL53L0X":"not found",initialized);
      checked=true;last_frame=esp_timer_get_time()/1000;
    }
    GridFrame f;bool publish=false;int mm=0,status=0;bool valid=false;
    if(initialized&&use_grid){
      uint8_t ready=0;uint8_t error=vl53l7cx_check_data_ready(&dev,&ready);
      if(!error&&ready){error=vl53l7cx_get_ranging_data(&dev,&results);
        if(!error){publish=true;f.connected=true;for(int i=0;i<64;++i){f.mm[i]=results.distance_mm[i];f.status[i]=results.nb_target_detected[i]?results.target_status[i]:255;}}
      }
      if(error || uint32_t(esp_timer_get_time()/1000-last_frame)>500){initialized=false;publish=true;}
    }else if(initialized){
      mm=old.readRangeContinuousMillimeters();status=old.range_status;
      valid=Wire.healthy&&!old.timeoutOccurred()&&status==11&&mm>=20&&mm<=2000;
      publish=true;if(!Wire.healthy)initialized=false;
    }else publish=true;
    if(publish){
      uint32_t now=esp_timer_get_time()/1000;last_frame=now;
      xSemaphoreTake(state_mutex,portMAX_DELAY);
      grid_sensor_present=use_grid;f.at=now;f.sequence=++sequence;grid_frame=f;
      auto previous=grid_match;grid_match=match_grid(grid_store,f);
      if(grid_match.recognized && previous.recognized && previous.profile==grid_match.profile)
        grid_stable_frames=std::min(5,grid_stable_frames+1);
      else grid_stable_frames=grid_match.recognized?1:0;
      if(use_grid){
        // Only trained interior zones drive automatic fill; boundary/tray zones
        // remain diagnostic. The nearest interior return preserves raw cutoff.
        mm=grid_match.nearest;status=5;
        valid=grid_store.enabled && grid_match.recognized && grid_match.nearest>=20;
      }
      Sample s{mm,status,valid,now,sequence};
      s.filtered_mm=filter.update(mm,valid,now,settings.stale_ms);s.filtered_valid=valid;sample=s;
      xSemaphoreGive(state_mutex);
    }
    vTaskDelay(pdMS_TO_TICKS(initialized?20:1000));
  }
}
} // namespace tap
