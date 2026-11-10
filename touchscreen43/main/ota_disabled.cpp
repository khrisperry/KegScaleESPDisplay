#include "app.h"
#include "esp_log.h"
esp_err_t touchscreen_ota(bool) {
  ESP_LOGW("touchscreen43", "Temporary 4.3-inch test build: USB updates only");
  ui_message("4.3-inch test build: update using USB only.");
  return ESP_ERR_NOT_SUPPORTED;
}
