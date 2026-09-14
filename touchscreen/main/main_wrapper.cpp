#include "app.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <atomic>
#include <cstdio>
#include <cstring>

extern "C" void touchscreen_request_auto_discovery();

/* Keep OTA's real esp_restart() in touchscreen_ota.cpp. Only the settings-save
 * restart inside main.cpp is redirected to a live reconfiguration routine. */
static void touchscreen_apply_settings_live();
static void touchscreen_ui_message(const char *message);

#define app_main touchscreen_app_main
#define esp_restart touchscreen_apply_settings_live
#define ui_message touchscreen_ui_message
#include "main.cpp"
#undef ui_message
#undef esp_restart
#undef app_main

namespace {
std::atomic<bool> discovery_running{false};

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
    snprintf((char *)desired.sta.ssid, sizeof(desired.sta.ssid), "%s",
             settings.ssid);
    snprintf((char *)desired.sta.password, sizeof(desired.sta.password), "%s",
             settings.password);

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
