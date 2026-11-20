#include "tap_control.h"
#include "freertos/task.h"
#include "esp_log.h"
extern "C" void touchscreen_app_main();
void touchscreen_start_pairing_guard();
static void display_task(void *) { touchscreen_app_main(); vTaskDelete(nullptr); }
extern "C" void app_main() {
  tap::start(); // Calibrated closed command and controller before display/Wi-Fi.
  configASSERT(xTaskCreate(display_task, "touchpour_app", 16384, nullptr, 5, nullptr) == pdPASS);
  touchscreen_start_pairing_guard();
}
