"""Run the real flash history implementation against simulated ESP flash/RTOS."""
import pathlib
import subprocess
import tempfile
root = pathlib.Path(__file__).resolve().parents[1]
mock = r'''
#pragma once
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <stdexcept>
using esp_err_t = int;
constexpr int ESP_OK = 0, ESP_FAIL = -1, pdPASS = 1, pdTRUE = 1, portMAX_DELAY = 0;
using SemaphoreHandle_t = void *;
using TaskHandle_t = void *;
#define configASSERT(x) assert(x)
#define ESP_LOGI(...) do {} while (0)
#define ESP_LOGE(...) do {} while (0)
inline const char *esp_err_to_name(int) { return "error"; }
inline SemaphoreHandle_t xSemaphoreCreateMutex() { return (void *)1; }
inline void xSemaphoreTake(void *, int) {}
inline void xSemaphoreGive(void *) {}
struct Done {};
inline int notifications = 0;
inline void (*task_fn)(void *);
inline int xTaskCreate(void (*fn)(void *), const char *, int, void *, int, void **out) { task_fn = fn; *out = (void *)1; return 1; }
inline int ulTaskNotifyTake(int, int) { if (notifications++) throw Done{}; return 1; }
inline void xTaskNotifyGive(void *) { notifications = 0; try { task_fn(nullptr); } catch (Done &) {} }
struct Description { const char *version; };
inline const Description *esp_app_get_description() { static Description d{"0.4.0"}; return &d; }
using esp_partition_subtype_t = int;
constexpr int ESP_PARTITION_TYPE_DATA = 1;
struct esp_partition_t { unsigned address, size; };
inline esp_partition_t fake_partition{0xc20000, 0x18000};
inline unsigned char flash[0x18000];
inline int writes = 0, fail_write = -1;
inline const esp_partition_t *esp_partition_find_first(int, int, const char *) { return nullptr; }
inline int esp_partition_register_external(void *, unsigned address, unsigned size, const char *, int, int, const esp_partition_t **out) { assert(address == fake_partition.address && size == fake_partition.size); *out = &fake_partition; return 0; }
inline int esp_partition_read(const esp_partition_t *, size_t at, void *out, size_t n) { assert(at + n <= sizeof(flash)); memcpy(out, flash + at, n); return 0; }
inline int esp_partition_erase_range(const esp_partition_t *, size_t at, size_t n) { assert(at + n <= sizeof(flash) && at % 4096 == 0 && n % 4096 == 0); memset(flash + at, 255, n); return 0; }
inline int esp_partition_write(const esp_partition_t *, size_t at, const void *in, size_t n) { assert(at + n <= sizeof(flash)); if (++writes == fail_write) return -1; const auto *p = (const unsigned char *)in; for (size_t i=0;i<n;++i) { assert((flash[at+i] & p[i]) == p[i]); flash[at+i] &= p[i]; } return 0; }
'''
harness = r'''
#include "pour_history.cpp"
using namespace tap;
void restart() {
  for (auto &h : headers) { h = {}; }
  next_id = 1; recording = false;
  history_saving = false; history_init();
}
void pour(bool hold=false) {
  Settings c; c.stop_mm=75; c.servo_calibrated=c.distance_calibrated=true;
  history_observe(Sample{200,0,true,1000,1},c,1000,2500,true,hold,true,"Pouring");
  history_observe(Sample{150,0,true,1490,2},c,1490,2500,true,hold,false,"Pouring");
  history_observe(Sample{100,0,true,1500,3},c,1500,2500,true,hold,false,"Pouring");
  history_observe(Sample{75,0,true,1610,4},c,1610,1000,false,false,false,"Fill target reached");
}
int main() {
  memset(flash,255,sizeof(flash)); history_init();
  PourSummary list[5]; assert(history_available() && history_list(list)==0);
  pour(); assert(history_list(list)==1 && list[0].id==1 && list[0].samples==3);
  assert(list[0].settings.stop_mm==75 && list[0].duration_ms==610);
  PourPoint row; assert(history_point(1,2,row) && row.mm==75 && row.servo_us==1000);
  assert(!history_point(1,3,row));
  for(int i=0;i<6;++i) pour(i==5);
  assert(history_list(list)==5 && list[0].id==7 && list[4].id==3 && list[0].manual_hold);
  assert(!history_get(2,list[0]));
  restart(); assert(history_list(list)==5 && list[0].id==7 && list[4].id==3);
  // Fail header body, then final commit; each must preserve the latest five.
  for(int fail : {2,3}) {
    writes=0; fail_write=fail; pour(); fail_write=-1;
    restart(); assert(history_list(list)==5 && list[0].id==7 && list[4].id==3);
  }
  pour(); restart(); assert(history_list(list)==5 && list[0].id==8 && list[4].id==4);
  // Corrupt payload: CRC rejects it at boot rather than serving bad measurements.
  for(size_t i=0;i<slot_count;++i) if(headers[i].summary.id==8) flash[i*slot_size+sizeof(Header)]^=1;
  restart(); assert(history_list(list)==5 && list[0].id==7);
  // The longest configured pour retains its initial and final readings.
  Settings c; c.max_pour_ms=120000;
  history_observe(Sample{200,0,true,0,1},c,0,2500,true,false,false,"Pouring");
  for(uint32_t t=500;t<120000;t+=500) history_observe(Sample{100,0,true,t,t/500+1},c,t,2500,true,false,false,"Pouring");
  history_observe(Sample{100,0,true,120000,241},c,120000,1000,false,false,false,"Maximum pour time");
  assert(history_list(list)==5 && list[0].samples==241);
  assert(history_point(list[0].id,240,row) && row.elapsed_ms==120000 && row.servo_us==1000);
  puts("PASS: real history sampling/settings/final reading, last-five rotation, reboot recovery, torn writes, CRC rejection, maximum duration");
}
'''
with tempfile.TemporaryDirectory() as directory:
    d = pathlib.Path(directory)
    (d/'mock.h').write_text(mock)
    for name in ['esp_app_desc.h','esp_partition.h','esp_log.h','freertos/FreeRTOS.h','freertos/task.h','freertos/semphr.h']:
        p=d/name; p.parent.mkdir(parents=True,exist_ok=True); p.write_text('#include "mock.h"\n')
    (d/'test.cpp').write_text(harness)
    subprocess.run(['g++','-std=c++17','-Wall','-Wextra','-Werror','-Wno-unused-variable','-I',str(d),'-I',str(root/'components/tap_control'),str(d/'test.cpp'),'-o',str(d/'test')],check=True)
    subprocess.run([str(d/'test')],check=True)
