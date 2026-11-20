#include "../../../touchscreen/main/app.h"
#include "nvs.h"
#include <cstdio>
// Web Wi-Fi updates use the same saved settings as the on-screen Setup.
// Caller holds tap's state mutex and reboots after the calibrated tap closes.
esp_err_t touchpour_save_wifi(const char *ssid, const char *password) {
  nvs_handle_t h;
  esp_err_t e = nvs_open("touchscreen", NVS_READWRITE, &h);
  if (e != ESP_OK) return e;
  Settings saved{};
  size_t bytes = sizeof(saved);
  e = nvs_get_blob(h, "settings", &saved, &bytes);
  if (e == ESP_ERR_NVS_NOT_FOUND) { saved.brightness = 85; e = ESP_OK; }
  if (e == ESP_OK && bytes == sizeof(saved)) {
    snprintf(saved.ssid, sizeof(saved.ssid), "%s", ssid);
    snprintf(saved.password, sizeof(saved.password), "%s", password);
    e = nvs_set_blob(h, "settings", &saved, sizeof(saved));
    if (e == ESP_OK) e = nvs_commit(h);
  } else if (e == ESP_OK) e = ESP_ERR_INVALID_SIZE;
  nvs_close(h);
  return e;
}
