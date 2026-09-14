#include "app.h"
#include "esp_heap_caps.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <atomic>
#include <cstdio>
#include <cstring>

extern "C" void touchscreen_request_auto_discovery();

/* Keep OTA's real esp_restart() in touchscreen_ota.cpp. Only the settings-save
 * restart inside main.cpp is redirected to a live reconfiguration routine.
 * OTA itself is redirected to a dedicated task so HTTPS/TLS and flash writes
 * never run on the long-lived touchscreen application stack. */
static void touchscreen_apply_settings_live();
static void touchscreen_ui_message(const char *message);
static esp_err_t touchscreen_start_ota_task();

#define app_main touchscreen_app_main
#define esp_restart touchscreen_apply_settings_live
#define ui_message touchscreen_ui_message
#define touchscreen_ota touchscreen_start_ota_task
#include "main.cpp"
#undef touchscreen_ota
#undef ui_message
#undef esp_restart
#undef app_main

namespace {
std::atomic<bool> discovery_running{false};
std::atomic<bool> ota_running{false};
/* ESP32-S3 task stacks must come from internal RAM. The previous 32 KB worker
 * could not be created once Wi-Fi/LVGL were running. OTA's large transfer
 * buffers now live in PSRAM, so a 12 KB internal stack is sufficient. */
constexpr uint32_t kOtaTaskStackBytes = 12 * 1024;
constexpr UBaseType_t kOtaTaskPriority = 4;

bool wifi_settings_changed() {
  wifi_config_t active = {};
  if (esp_wifi_get_config(WIFI_IF_STA, &active) != ESP_OK)
    return true;
  return strncmp((const char *)active.sta.ssid, settings.ssid,
                 sizeof(active.sta.ssid)) != 0 ||
         strncmp((const char *)active.sta.password, settings.password,
                 sizeof(active.sta.password)) != 0;
}

bool scale_host_changed() {
  char desired[sizeof(uri)] = {};
  if (settings.host[0])
    snprintf(desired, sizeof(desired), "ws://%s/ws/controller", settings.host);
  return strcmp(uri, desired) != 0;
}

void ota_task(void *) {
  vTaskDelay(pdMS_TO_TICKS(250));
  ESP_LOGI(TAG,
           "Touchscreen OTA worker started; stack high-water=%u bytes",
           (unsigned)uxTaskGetStackHighWaterMark(nullptr));
  ::ui_message("Checking for touchscreen update…");

  esp_err_t result = ::touchscreen_ota();

  ESP_LOGI(TAG,
           "Touchscreen OTA worker finished: %s; stack high-water=%u bytes",
           esp_err_to_name(result),
           (unsigned)uxTaskGetStackHighWaterMark(nullptr));
  if (result != ESP_OK)
    ::ui_message(esp_err_to_name(result));

  ota_running = false;
  vTaskDelete(nullptr);
}

void auto_discovery_task(void *) {
  /* DHCP can take a few seconds after first-time Wi-Fi setup. */
  for (int i = 0; i < 40 && !wifi_ready; ++i)
    vTaskDelay(pdMS_TO_TICKS(500));

  if (!wifi_ready || settings.host[0]) {
    discovery_running = false;
    vTaskDelete(nullptr);
    return;
  }

  touchscreen_ui_message("Wi-Fi connected — looking for your scale…");
  mdns_result_t *found = nullptr;
  esp_err_t e = mdns_query_ptr("_kegscale", "_tcp", 3000, 4, &found);
  unsigned count = 0;
  mdns_result_t *only = nullptr;
  for (auto p = found; p; p = p->next) {
    if (p->hostname) {
      ++count;
      only = p;
    }
  }

  if (e == ESP_OK && count == 1 && only) {
    snprintf(settings.host, sizeof(settings.host), "%s.local", only->hostname);
    if (persist() == ESP_OK) {
      ui_settings_applied(settings);
      touchscreen_ui_message("Scale found automatically — connecting…");
      retry_connection = true;
      next_connection_attempt = now();
      connect_scale();
    } else {
      touchscreen_ui_message("Scale found, but its address could not be saved");
    }
  } else if (count > 1) {
    touchscreen_ui_message("Several scales found — choose one in Setup");
  } else {
    touchscreen_ui_message("No scale found — use Find Scale or enter its address");
  }

  if (found)
    mdns_query_results_free(found);
  discovery_running = false;
  vTaskDelete(nullptr);
}
} // namespace

static esp_err_t touchscreen_start_ota_task() {
  if (ota_running.exchange(true)) {
    ESP_LOGW(TAG, "Touchscreen OTA request ignored because an update is already running");
    return ESP_ERR_INVALID_STATE;
  }

  const size_t internal_free =
      heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  const size_t internal_largest =
      heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  const size_t psram_free =
      heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  ESP_LOGI(TAG,
           "Scheduling touchscreen OTA worker; caller stack high-water=%u bytes; internal_free=%u largest_internal=%u psram_free=%u requested_stack=%u",
           (unsigned)uxTaskGetStackHighWaterMark(nullptr),
           (unsigned)internal_free, (unsigned)internal_largest,
           (unsigned)psram_free, (unsigned)kOtaTaskStackBytes);

  BaseType_t created = xTaskCreate(ota_task, "touchscreen_ota",
                                   kOtaTaskStackBytes, nullptr,
                                   kOtaTaskPriority, nullptr);
  if (created != pdPASS) {
    ota_running = false;
    ESP_LOGE(TAG,
             "Could not create touchscreen OTA worker task: largest_internal=%u requested_stack=%u",
             (unsigned)internal_largest, (unsigned)kOtaTaskStackBytes);
    return ESP_ERR_NO_MEM;
  }
  return ESP_OK;
}

static void touchscreen_ui_message(const char *message) {
  if (message && !strcmp(message, "Settings saved — restarting"))
    ::ui_message("Applying settings…");
  else
    ::ui_message(message);
}

static void touchscreen_apply_settings_live() {
  const bool wifi_changed = wifi_settings_changed();
  const bool host_changed = scale_host_changed();

  /* The settings were already persisted by main.cpp before this hook runs.
   * Mirror them into the Setup UI immediately instead of relying on a reboot to
   * reload NVS. */
  ui_settings_applied(settings);

  if (!wifi_changed && !host_changed) {
    touchscreen_ui_message("Settings saved");
    return;
  }

  if (wifi_changed) {
    wifi_config_t desired = {};
    const size_t ssid_len = strnlen(settings.ssid, sizeof(desired.sta.ssid));
    const size_t password_len =
        strnlen(settings.password, sizeof(desired.sta.password));
    memcpy(desired.sta.ssid, settings.ssid, ssid_len);
    memcpy(desired.sta.password, settings.password, password_len);

    esp_err_t e = esp_wifi_set_config(WIFI_IF_STA, &desired);
    if (e != ESP_OK) {
      ESP_LOGW(TAG, "Could not apply Wi-Fi settings live: %s",
               esp_err_to_name(e));
      touchscreen_ui_message("Settings saved, but Wi-Fi could not be applied");
      return;
    }

    retry_connection = true;
    next_connection_attempt = now() + 3000000;
    wifi_ready = false;
    if (ws)
      esp_websocket_client_stop(ws);
    disconnected();

    esp_err_t d = esp_wifi_disconnect();
    if (d == ESP_ERR_WIFI_NOT_CONNECT && settings.ssid[0])
      esp_wifi_connect();

    touchscreen_ui_message(settings.ssid[0]
                               ? "Wi-Fi settings saved — reconnecting…"
                               : "Wi-Fi settings saved");
    if (!settings.host[0])
      touchscreen_request_auto_discovery();
    return;
  }

  /* Same Wi-Fi, different scale. Rebuild only the controller connection. */
  retry_connection = true;
  next_connection_attempt = now();
  touchscreen_ui_message(settings.host[0]
                             ? "Scale setting saved — reconnecting…"
                             : "Scale address cleared");
  connect_scale();
  if (!settings.host[0])
    touchscreen_request_auto_discovery();
}

extern "C" void touchscreen_request_auto_discovery() {
  if (settings.host[0] || discovery_running.exchange(true))
    return;
  BaseType_t created = xTaskCreate(auto_discovery_task, "scale_discovery", 6144,
                                   nullptr, 4, nullptr);
  if (created != pdPASS) {
    discovery_running = false;
    ESP_LOGW(TAG, "Could not create automatic scale discovery task");
  }
}
