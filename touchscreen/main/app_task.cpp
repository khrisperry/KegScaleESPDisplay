#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

extern "C" void touchscreen_app_main();
extern "C" void touchscreen_request_auto_discovery();

namespace {
constexpr uint32_t kTouchscreenTaskStackBytes = 16 * 1024;
constexpr UBaseType_t kTouchscreenTaskPriority = 5;

void touchscreen_task(void *) {
  touchscreen_app_main();
  ESP_LOGE("wifi_touchscreen", "Touchscreen application task returned unexpectedly");
  vTaskDelete(nullptr);
}
} // namespace

extern "C" void app_main() {
  BaseType_t created =
      xTaskCreate(touchscreen_task, "touchscreen_app",
                  kTouchscreenTaskStackBytes, nullptr,
                  kTouchscreenTaskPriority, nullptr);
  configASSERT(created == pdPASS);
  touchscreen_request_auto_discovery();
}
