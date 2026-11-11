#pragma once
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdint.h>
using boolean = bool;
#include <stdlib.h>
inline uint32_t millis() {
  vTaskDelay(1);
  return esp_timer_get_time() / 1000;
}
