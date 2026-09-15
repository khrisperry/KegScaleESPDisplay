#include "app.h"
#include "controller_link.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>

extern "C" void touchscreen_request_auto_discovery();

/* Keep OTA's real esp_restart() in touchscreen_ota.cpp. Only the settings-save
 * restart inside main.cpp is redirected to a live reconfiguration routine.
 * Legacy OTA calls are redirected to a dedicated task; the new OTA UI calls
 * touchscreen_ota_request() directly so update checks never disturb the scale
 * WebSocket session.
 *
 * Pairing synchronization is layered around the legacy app as well. Clearing
 * the saved master key first asks the scale to remove its matching pairing,
 * and authenticated inbound traffic is inspected for the scale's final
 * encrypted unpair notification. */
static void touchscreen_apply_settings_live();
static void touchscreen_ui_message(const char *message);
static esp_err_t touchscreen_start_ota_task_legacy();
static bool touchscreen_remove_scale_pairing(const char *host);
static void *touchscreen_pairing_memset(void *dest, int value, size_t count);
static esp_err_t touchscreen_pairing_cl_open(cl_session_t *session,
                                             const uint8_t *in, size_t len,
                                             char *out);

#define app_main touchscreen_app_main
#define esp_restart touchscreen_apply_settings_live
#define ui_message touchscreen_ui_message
#define touchscreen_ota touchscreen_start_ota_task_legacy
#define memset touchscreen_pairing_memset
#define cl_open touchscreen_pairing_cl_open
#include "main.cpp"
#undef cl_open
#undef memset
#undef touchscreen_ota
#undef ui_message
#undef esp_restart
#undef app_main

namespace {
std::atomic<bool> discovery_running{false};
std::atomic<bool> ota_running{false};
std::atomic<bool> ota_install_requested{false};
std::atomic<bool> ota_foreground_requested{true};
std::atomic<bool> ota_scheduler_started{false};
std::atomic<bool> ota_preferences_loaded{false};
std::atomic<int> ota_channel_index{2}; // dev for existing development devices
std::atomic<bool> ota_auto_install{true};
std::atomic<int64_t> ota_last_check_us{0};

constexpr uint32_t kOtaTaskStackBytes = 12 * 1024;
constexpr UBaseType_t kOtaTaskPriority = 4;
constexpr uint32_t kInitialOtaCheckDelayMs = 30000;
constexpr uint32_t kDailyOtaCheckMs = 24U * 60U * 60U * 1000U;
constexpr const char *kOtaNvsNamespace = "touch_ota";
constexpr const char *kOtaChannelKey = "channel";
constexpr const char *kOtaAutoInstallKey = "auto_install";

const char *channel_name(int index) {
  switch (index) {
  case 0:
    return "production";
  case 1:
    return "beta";
  default:
    return "dev";
  }
}

int channel_index(const char *channel) {
  if (channel && !strcmp(channel, "production"))
    return 0;
  if (channel && !strcmp(channel, "beta"))
    return 1;
  if (channel && !strcmp(channel, "dev"))
    return 2;
  return -1;
}

void load_ota_preferences() {
  if (ota_preferences_loaded.exchange(true))
    return;

  int index = 2;
  bool auto_install = true;
  nvs_handle_t nvs;
  esp_err_t e = nvs_open(kOtaNvsNamespace, NVS_READONLY, &nvs);
  if (e == ESP_OK) {
    char channel[16] = {};
    size_t length = sizeof(channel);
    if (nvs_get_str(nvs, kOtaChannelKey, channel, &length) == ESP_OK) {
      int saved = channel_index(channel);
      if (saved >= 0)
        index = saved;
    }
    uint8_t saved_auto = 1;
    if (nvs_get_u8(nvs, kOtaAutoInstallKey, &saved_auto) == ESP_OK)
      auto_install = saved_auto != 0;
    nvs_close(nvs);
  }

  ota_channel_index = index;
  ota_auto_install = auto_install;
  ESP_LOGI(TAG, "Loaded touchscreen OTA settings: channel=%s auto_install=%d",
           channel_name(index), auto_install);
}

bool wifi_settings_changed() {
  wifi_config_t active = {};
  if (esp_wifi_get_config(WIFI_IF_STA, &active) != ESP_OK)
    return true;
  return strncmp((const char *)active.sta.ssid, settings.ssid,
                 sizeof(active.sta.ssid)) != 0 ||
         strncmp((const char *)active.sta.password, settings.password,
                 sizeof(active.sta.password)) != 0;
}

bool scale_host_changed(uint8_t slot) {
  const auto &c = connection_for_const(slot);
  char desired[180] = {};
  if (scale_host_const(slot)[0])
    snprintf(desired, sizeof(desired), "ws://%s/ws/controller",
             scale_host_const(slot));
  return strcmp(c.uri, desired) != 0;
}

void ota_task(void *) {
  const bool install = ota_install_requested.load();
  const bool foreground = ota_foreground_requested.load();
  vTaskDelay(pdMS_TO_TICKS(250));
  ESP_LOGI(TAG,
           "Touchscreen OTA worker started: mode=%s foreground=%d stack high-water=%u bytes",
           install ? "install" : "check", foreground,
           (unsigned)uxTaskGetStackHighWaterMark(nullptr));

  if (foreground && !install)
    ui_update_checking();

  esp_err_t result = ::touchscreen_ota(install);
  ota_last_check_us = esp_timer_get_time();

  ESP_LOGI(TAG,
           "Touchscreen OTA worker finished: mode=%s result=%s stack high-water=%u bytes",
           install ? "install" : "check", esp_err_to_name(result),
           (unsigned)uxTaskGetStackHighWaterMark(nullptr));
  if (result != ESP_OK) {
    ui_update_error(install ? "Firmware update failed. Check Wi-Fi and try again."
                            : "Could not check for updates. Check Wi-Fi and try again.",
                    install);
  }

  ota_running = false;
  vTaskDelete(nullptr);
}

void ota_scheduler_task(void *) {
  /* Wait until the legacy application has initialized NVS and networking. */
  while (!actions)
    vTaskDelay(pdMS_TO_TICKS(250));
  load_ota_preferences();

  while (!wifi_ready)
    vTaskDelay(pdMS_TO_TICKS(1000));
  vTaskDelay(pdMS_TO_TICKS(kInitialOtaCheckDelayMs));

  for (;;) {
    if (wifi_ready) {
      const bool install = ota_auto_install.load();
      esp_err_t e = touchscreen_ota_request(install, false);
      if (e == ESP_ERR_INVALID_STATE)
        ESP_LOGI(TAG, "Automatic OTA check skipped because another check is running");
      else if (e != ESP_OK)
        ESP_LOGW(TAG, "Could not schedule automatic OTA check: %s",
                 esp_err_to_name(e));
    }
    vTaskDelay(pdMS_TO_TICKS(kDailyOtaCheckMs));
  }
}

void auto_discovery_task(void *) {
  for (int i = 0; i < 40 && !wifi_ready; ++i)
    vTaskDelay(pdMS_TO_TICKS(500));

  if (!wifi_ready || active_host_const()[0]) {
    discovery_running = false;
    vTaskDelete(nullptr);
    return;
  }

  touchscreen_ui_message("Wi-Fi connected - looking for your scale...");
  mdns_result_t *found = nullptr;
  esp_err_t e = mdns_query_ptr("_kegscale", "_tcp", 3000, 8, &found);
  char options[800];
  const unsigned count =
      e == ESP_OK
          ? build_scale_discovery_options(found, options, sizeof(options))
          : 0;
  if (e != ESP_OK)
    snprintf(options, sizeof(options), "Manual IP / hostname...");

  if (e == ESP_OK && count == 1) {
    char host[128] = {};
    const char *line_end = strchr(options, '\n');
    const size_t length =
        line_end ? std::min(sizeof(host) - 1,
                            (size_t)(line_end - options))
                 : std::min(sizeof(host) - 1, strlen(options));
    memcpy(host, options, length);
    host[length] = 0;
    snprintf(active_host(), 128, "%s", host);
    if (persist() == ESP_OK) {
      publish_scale_profiles();
      ui_settings_applied(settings);
      touchscreen_ui_message("Scale found automatically - connecting...");
      auto &c = connection_for(active_scale_index);
      c.retry_connection = true;
      c.next_connection_attempt = now();
      connect_scale(active_scale_index);
    } else {
      touchscreen_ui_message("Scale found, but its address could not be saved");
    }
  } else if (count > 1) {
    ui_discovered_options(options);
    touchscreen_ui_message("Several scales found - choose one in Setup");
  } else {
    ui_discovered_options(options);
    touchscreen_ui_message("No scale found - choose Manual IP / hostname");
  }

  if (found)
    mdns_query_results_free(found);
  discovery_running = false;
  vTaskDelete(nullptr);
}
} // namespace

static bool touchscreen_remove_scale_pairing(const char *host) {
  if (!host || !host[0] || !wifi_ready)
    return false;

  char url[192];
  snprintf(url, sizeof(url), "http://%s/api/controller", host);
  static const char body[] = "{\"action\":\"remove\"}";
  esp_http_client_config_t cfg = {};
  cfg.url = url;
  cfg.timeout_ms = 3000;
  auto client = esp_http_client_init(&cfg);
  if (!client)
    return false;

  esp_http_client_set_method(client, HTTP_METHOD_POST);
  esp_http_client_set_header(client, "Content-Type", "application/json");
  esp_http_client_set_header(client, "X-Controller-Setup", "1");
  esp_http_client_set_post_field(client, body, strlen(body));
  esp_err_t e = esp_http_client_perform(client);
  int status = e == ESP_OK ? esp_http_client_get_status_code(client) : 0;
  esp_http_client_cleanup(client);

  ESP_LOGI(TAG, "Scale pairing removal request: host='%s' result=%s status=%d",
           host, esp_err_to_name(e), status);
  return e == ESP_OK && status == 200;
}

static void *touchscreen_pairing_memset(void *dest, int value, size_t count) {
  const int slot = scale_index_for_master(dest);
  const bool clearing_pairing = slot >= 0 && value == 0 && count == 32;
  if (clearing_pairing && scale_host_const((uint8_t)slot)[0]) {
    if (touchscreen_remove_scale_pairing(scale_host_const((uint8_t)slot)))
      ESP_LOGI(TAG, "Matching scale %u pairing removed",
               (unsigned)(slot + 1));
    else
      ESP_LOGW(TAG,
               "Could not immediately remove scale %u pairing; mismatch cleanup will retry when reachable",
               (unsigned)(slot + 1));
  }
  return std::memset(dest, value, count);
}

static esp_err_t touchscreen_pairing_cl_open(cl_session_t *session,
                                             const uint8_t *in, size_t len,
                                             char *out) {
  esp_err_t e = cl_open(session, in, len, out);
  if (e != ESP_OK || !out)
    return e;

  cJSON *message = cJSON_Parse(out);
  if (message && !strcmp(str(message, "type"), "unpair")) {
    const int found_slot = scale_index_for_link(session);
    if (found_slot >= 0) {
      const uint8_t slot = (uint8_t)found_slot;
      const uint8_t other = slot == 0 ? 1 : 0;
      const bool removed_active = slot == active_scale_index;
      ESP_LOGI(TAG, "Scale %u requested synchronized touchscreen unpair",
               (unsigned)(slot + 1));
      scale_paired(slot) = false;
      std::memset(scale_master(slot), 0, 32);
      auto &c = connection_for(slot);
      c.retry_connection = false;
      c.state.online = false;
      c.authenticated = false;
      c.traffic_ready = false;
      if (removed_active && scale_slot_ready(other))
        active_scale_index = other;

      esp_err_t saved = persist();
      publish_scale_profiles();
      if (saved != ESP_OK)
        ESP_LOGE(TAG, "Could not persist scale %u requested unpair: %s",
                 (unsigned)(slot + 1), esp_err_to_name(saved));

      if (removed_active) {
        publish_active_state();
        ::ui_message(scale_slot_ready(other)
                         ? "Pairing removed; switched to the other scale."
                         : "Pairing removed on scale. Open Setup to pair again.");
      }
    }
  }
  cJSON_Delete(message);
  return e;
}

void touchscreen_ota_get_preferences(OtaPreferences *preferences) {
  if (!preferences)
    return;
  load_ota_preferences();
  snprintf(preferences->channel, sizeof(preferences->channel), "%s",
           channel_name(ota_channel_index.load()));
  preferences->auto_install = ota_auto_install.load();
}

esp_err_t touchscreen_ota_save_preferences(const char *channel,
                                           bool auto_install) {
  const int index = channel_index(channel);
  if (index < 0)
    return ESP_ERR_INVALID_ARG;

  nvs_handle_t nvs;
  esp_err_t e = nvs_open(kOtaNvsNamespace, NVS_READWRITE, &nvs);
  if (e != ESP_OK)
    return e;
  e = nvs_set_str(nvs, kOtaChannelKey, channel_name(index));
  if (e == ESP_OK)
    e = nvs_set_u8(nvs, kOtaAutoInstallKey, auto_install ? 1 : 0);
  if (e == ESP_OK)
    e = nvs_commit(nvs);
  nvs_close(nvs);
  if (e != ESP_OK)
    return e;

  ota_channel_index = index;
  ota_auto_install = auto_install;
  ota_preferences_loaded = true;
  ESP_LOGI(TAG, "Saved touchscreen OTA settings: channel=%s auto_install=%d",
           channel_name(index), auto_install);
  return ESP_OK;
}

void touchscreen_ota_get_channel(char *channel, size_t size) {
  if (!channel || !size)
    return;
  load_ota_preferences();
  snprintf(channel, size, "%s", channel_name(ota_channel_index.load()));
}

bool touchscreen_ota_has_checked(void) { return ota_last_check_us.load() > 0; }

uint64_t touchscreen_ota_last_check_age_seconds(void) {
  const int64_t checked = ota_last_check_us.load();
  if (checked <= 0)
    return 0;
  const int64_t age = esp_timer_get_time() - checked;
  return age > 0 ? (uint64_t)(age / 1000000LL) : 0;
}

bool touchscreen_ota_auto_install_enabled(void) {
  load_ota_preferences();
  return ota_auto_install.load();
}

void touchscreen_set_ota_install_mode(bool install) {
  ota_install_requested = install;
}

esp_err_t touchscreen_ota_request(bool install, bool foreground) {
  if (ota_running.exchange(true)) {
    ESP_LOGW(TAG,
             "Touchscreen OTA request ignored because an update is already running");
    return ESP_ERR_INVALID_STATE;
  }

  ota_install_requested = install;
  ota_foreground_requested = foreground;
  const size_t internal_free =
      heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  const size_t internal_largest = heap_caps_get_largest_free_block(
      MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  const size_t psram_free =
      heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  ESP_LOGI(TAG,
           "Scheduling touchscreen OTA worker: mode=%s foreground=%d caller stack high-water=%u bytes; internal_free=%u largest_internal=%u psram_free=%u requested_stack=%u",
           install ? "install" : "check", foreground,
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
    if (foreground || install)
      ui_update_error(install ? "Not enough memory to start the firmware update."
                              : "Not enough memory to check for updates.",
                      install);
    return ESP_ERR_NO_MEM;
  }
  return ESP_OK;
}

static esp_err_t touchscreen_start_ota_task_legacy() {
  return touchscreen_ota_request(ota_install_requested.load(), true);
}

void touchscreen_start_ota_scheduler(void) {
  if (ota_scheduler_started.exchange(true))
    return;
  BaseType_t created = xTaskCreate(ota_scheduler_task, "touch_ota_schedule", 4096,
                                   nullptr, 2, nullptr);
  if (created != pdPASS) {
    ota_scheduler_started = false;
    ESP_LOGW(TAG, "Could not create touchscreen OTA scheduler task");
  }
}

static void touchscreen_ui_message(const char *message) {
  if (message && !strcmp(message, "Settings saved — restarting")) {
    ::ui_message("Applying settings...");
    return;
  }

  if (message &&
      !strcmp(message,
              "In Setup: Remove pairing, then Add touchscreen on the scale.")) {
    const uint8_t slot = active_scale_index;
    if (scale_paired(slot)) {
      scale_paired(slot) = false;
      std::memset(scale_master(slot), 0, 32);
      if (persist() == ESP_OK) {
        publish_scale_profiles();
        ESP_LOGI(TAG,
                 "Cleared stale local scale %u pairing after scale-side removal",
                 (unsigned)(slot + 1));
      } else {
        ESP_LOGE(TAG, "Could not persist stale-pairing cleanup");
      }
    }
    auto &c = connection_for(slot);
    c.retry_connection = false;
    c.state.online = false;
    c.authenticated = false;
    c.traffic_ready = false;
    ui_state(c.state);
    ::ui_message("Pairing removed on scale. Open Setup to pair again.");
    return;
  }

  if (message &&
      !strcmp(message,
              "On scale: remove old Wi-Fi touchscreen, then Add touchscreen.")) {
    const uint8_t slot = active_scale_index;
    if (touchscreen_remove_scale_pairing(scale_host_const(slot))) {
      ESP_LOGI(TAG,
               "Cleared stale scale %u pairing after touchscreen-side removal",
               (unsigned)(slot + 1));
      ::ui_message(
          "Old scale pairing cleared. Select Add touchscreen to pair again.");
    } else {
      ::ui_message(
          "Scale still has the old pairing. Cleanup will retry automatically.");
    }
    return;
  }

  ::ui_message(message);
}

static void touchscreen_apply_settings_live() {
  const uint8_t slot = active_scale_index;
  const bool wifi_changed = wifi_settings_changed();
  const bool host_changed = scale_host_changed(slot);
  ui_settings_applied(settings);

  if (!wifi_changed && !host_changed) {
    publish_scale_profiles();
    publish_active_state();
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

    wifi_ready = false;
    for (uint8_t i = 0; i < 2; ++i)
      stop_scale_transport(i, scale_host_const(i)[0]);

    esp_err_t d = esp_wifi_disconnect();
    if (d == ESP_ERR_WIFI_NOT_CONNECT && settings.ssid[0])
      esp_wifi_connect();

    touchscreen_ui_message(settings.ssid[0]
                               ? "Wi-Fi settings saved - reconnecting..."
                               : "Wi-Fi settings saved");
    if (!active_host_const()[0])
      touchscreen_request_auto_discovery();
    return;
  }

  if (host_changed) {
    auto &c = connection_for(slot);
    c.retry_connection = scale_host_const(slot)[0];
    c.next_connection_attempt = now();
    touchscreen_ui_message(scale_host_const(slot)[0]
                               ? "Scale setting saved - connecting..."
                               : "Scale address cleared");
    if (scale_host_const(slot)[0])
      connect_scale(slot);
    else
      touchscreen_request_auto_discovery();
  } else {
    publish_active_state();
    touchscreen_ui_message("Scale selection saved");
  }
}

extern "C" void touchscreen_request_auto_discovery() {
  if (active_host_const()[0] || discovery_running.exchange(true))
    return;
  BaseType_t created = xTaskCreate(auto_discovery_task, "scale_discovery", 6144,
                                   nullptr, 4, nullptr);
  if (created != pdPASS) {
    discovery_running = false;
    ESP_LOGW(TAG, "Could not create automatic scale discovery task");
  }
}
