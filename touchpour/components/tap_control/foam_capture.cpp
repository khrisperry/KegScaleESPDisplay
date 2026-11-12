#include "foam_capture.h"
#include "esp_attr.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

namespace tap {
static EXT_RAM_BSS_ATTR FoamCapture capture;
static SemaphoreHandle_t capture_mutex;
void foam_init() {
  capture_mutex = xSemaphoreCreateMutex();
  configASSERT(capture_mutex);
}
void foam_start(uint32_t now) {
  xSemaphoreTake(capture_mutex, portMAX_DELAY);
  capture.start(now);
  xSemaphoreGive(capture_mutex);
}
void foam_stop(uint32_t now) {
  xSemaphoreTake(capture_mutex, portMAX_DELAY);
  capture.stop(now);
  xSemaphoreGive(capture_mutex);
}
bool foam_mark(uint8_t marker) {
  xSemaphoreTake(capture_mutex, portMAX_DELAY);
  bool result = capture.mark(marker);
  xSemaphoreGive(capture_mutex);
  return result;
}
FoamStatus foam_status(uint32_t now) {
  xSemaphoreTake(capture_mutex, portMAX_DELAY);
  FoamStatus result{capture.active, capture.count, capture.elapsed(now)};
  xSemaphoreGive(capture_mutex);
  return result;
}
bool foam_row(size_t index, FoamRow &out) {
  xSemaphoreTake(capture_mutex, portMAX_DELAY);
  bool result = index < capture.count;
  if (result) out = capture.rows[index];
  xSemaphoreGive(capture_mutex);
  return result;
}
void foam_observe(const Sample &s, const Settings &c, uint32_t now, int servo,
                  bool pouring, bool ready, bool test, bool button, int stable,
                  const char *reason, bool manual_hold) {
  xSemaphoreTake(capture_mutex, portMAX_DELAY);
  size_t before = capture.count;
  capture.observe(s, c, now, servo, pouring, ready, test, button, reason);
  if (capture.count != before) {
    capture.rows[before].stable_samples = stable;
    if (manual_hold) capture.rows[before].flags |= FoamCapture::MANUAL_HOLD;
  }
  xSemaphoreGive(capture_mutex);
}


} // namespace tap
