#include "app.h"
#include "cJSON.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"

#include <cstdio>
#include <cstring>

namespace {
const char *TAG = "pairing_guard";
constexpr uint32_t kGuardTaskStackBytes = 6144;
constexpr UBaseType_t kGuardTaskPriority = 3;
constexpr TickType_t kGuardPollTicks = pdMS_TO_TICKS(1000);
constexpr unsigned kMaxCleanupAttempts = 60;

struct StoredScaleProfile {
  char host[128];
  uint8_t master[32];
  bool paired;
};

struct SlotState {
  char host[128];
  bool paired;
};

struct PendingCleanup {
  bool active;
  char host[128];
  unsigned attempts;
};

bool load_slot_states(SlotState slots[2]) {
  nvs_handle_t nvs;
  esp_err_t e = nvs_open("touchscreen", NVS_READONLY, &nvs);
  if (e != ESP_OK)
    return false;

  Settings primary{};
  size_t primary_size = sizeof(primary);
  e = nvs_get_blob(nvs, "settings", &primary, &primary_size);
  if (e != ESP_OK || primary_size != sizeof(primary)) {
    nvs_close(nvs);
    return false;
  }

  StoredScaleProfile secondary{};
  size_t secondary_size = sizeof(secondary);
  esp_err_t secondary_result =
      nvs_get_blob(nvs, "scale2", &secondary, &secondary_size);
  if (secondary_result != ESP_OK && secondary_result != ESP_ERR_NVS_NOT_FOUND) {
    nvs_close(nvs);
    return false;
  }
  nvs_close(nvs);

  snprintf(slots[0].host, sizeof(slots[0].host), "%s", primary.host);
  slots[0].paired = primary.paired;
  if (secondary_result == ESP_OK && secondary_size == sizeof(secondary)) {
    snprintf(slots[1].host, sizeof(slots[1].host), "%s", secondary.host);
    slots[1].paired = secondary.paired;
  } else {
    slots[1] = {};
  }
  return true;
}

bool read_remote_paired(const char *host, bool *paired) {
  if (!host || !host[0] || !paired)
    return false;

  char url[192];
  char body[256] = {};
  snprintf(url, sizeof(url), "http://%s/api/controller", host);

  esp_http_client_config_t cfg = {};
  cfg.url = url;
  cfg.timeout_ms = 1500;
  esp_http_client_handle_t client = esp_http_client_init(&cfg);
  if (!client)
    return false;

  esp_err_t e = esp_http_client_open(client, 0);
  int status = 0;
  int received = -1;
  if (e == ESP_OK) {
    const int64_t headers = esp_http_client_fetch_headers(client);
    if (headers >= 0) {
      status = esp_http_client_get_status_code(client);
      received =
          esp_http_client_read_response(client, body, sizeof(body) - 1);
    }
  }
  esp_http_client_close(client);
  esp_http_client_cleanup(client);

  if (e != ESP_OK || status != 200 || received <= 0)
    return false;

  cJSON *json = cJSON_Parse(body);
  cJSON *value = json ? cJSON_GetObjectItemCaseSensitive(json, "paired") : nullptr;
  const bool valid = cJSON_IsBool(value);
  if (valid)
    *paired = cJSON_IsTrue(value);
  cJSON_Delete(json);
  return valid;
}

bool remove_remote_pairing(const char *host) {
  if (!host || !host[0])
    return false;

  char url[192];
  snprintf(url, sizeof(url), "http://%s/api/controller", host);
  static const char body[] = "{\"action\":\"remove\"}";

  esp_http_client_config_t cfg = {};
  cfg.url = url;
  cfg.timeout_ms = 2000;
  esp_http_client_handle_t client = esp_http_client_init(&cfg);
  if (!client)
    return false;

  esp_http_client_set_method(client, HTTP_METHOD_POST);
  esp_http_client_set_header(client, "Content-Type", "application/json");
  esp_http_client_set_header(client, "X-Controller-Setup", "1");
  esp_http_client_set_post_field(client, body, strlen(body));

  const esp_err_t e = esp_http_client_perform(client);
  const int status = e == ESP_OK ? esp_http_client_get_status_code(client) : 0;
  esp_http_client_cleanup(client);

  ESP_LOGI(TAG, "Cleanup POST host='%s' result=%s status=%d", host,
           esp_err_to_name(e), status);
  return e == ESP_OK && status == 200;
}

void start_cleanup(PendingCleanup *cleanup, const char *host, unsigned slot) {
  if (!cleanup || !host || !host[0])
    return;
  cleanup->active = true;
  cleanup->attempts = 0;
  snprintf(cleanup->host, sizeof(cleanup->host), "%s", host);
  ESP_LOGI(TAG,
           "Scale %u local pairing cleared; verifying scale-side cleanup for '%s'",
           slot + 1, cleanup->host);
}

void pairing_guard_task(void *) {
  SlotState previous[2] = {};
  PendingCleanup cleanup[2] = {};
  bool initialized = false;

  for (;;) {
    SlotState current[2] = {};
    if (!load_slot_states(current)) {
      vTaskDelay(kGuardPollTicks);
      continue;
    }

    if (!initialized) {
      previous[0] = current[0];
      previous[1] = current[1];
      initialized = true;
      ESP_LOGI(TAG, "Pairing cleanup guard initialized");
      vTaskDelay(kGuardPollTicks);
      continue;
    }

    for (unsigned slot = 0; slot < 2; ++slot) {
      if (previous[slot].paired && !current[slot].paired &&
          previous[slot].host[0]) {
        start_cleanup(&cleanup[slot], previous[slot].host, slot);
      }

      // A new pairing has already succeeded for this same scale. Never let a
      // delayed cleanup remove the newly established relationship.
      if (cleanup[slot].active && current[slot].paired &&
          !strcmp(current[slot].host, cleanup[slot].host)) {
        ESP_LOGI(TAG,
                 "Scale %u paired again before cleanup retry; canceling stale cleanup",
                 slot + 1);
        cleanup[slot] = {};
      }

      previous[slot] = current[slot];
    }

    for (unsigned slot = 0; slot < 2; ++slot) {
      PendingCleanup &pending = cleanup[slot];
      if (!pending.active)
        continue;

      if (pending.attempts >= kMaxCleanupAttempts) {
        ESP_LOGW(TAG,
                 "Scale %u pairing cleanup gave up after %u attempts for '%s'",
                 slot + 1, pending.attempts, pending.host);
        pending = {};
        continue;
      }

      ++pending.attempts;
      bool remote_paired = false;
      if (!read_remote_paired(pending.host, &remote_paired)) {
        if (pending.attempts == 1 || pending.attempts % 10 == 0)
          ESP_LOGW(TAG,
                   "Scale %u cleanup verification attempt %u could not reach '%s'",
                   slot + 1, pending.attempts, pending.host);
        continue;
      }

      if (!remote_paired) {
        ESP_LOGI(TAG,
                 "Scale %u pairing cleanup verified complete after %u attempt(s)",
                 slot + 1, pending.attempts);
        pending = {};
        continue;
      }

      ESP_LOGW(TAG,
               "Scale %u still reports an old pairing; removing it (attempt %u)",
               slot + 1, pending.attempts);
      if (remove_remote_pairing(pending.host)) {
        ESP_LOGI(TAG,
                 "Scale %u stale pairing removed; reconnect can begin immediately",
                 slot + 1);
        pending = {};
      }
    }

    vTaskDelay(kGuardPollTicks);
  }
}
} // namespace

void touchscreen_start_pairing_guard(void) {
  BaseType_t created = xTaskCreate(pairing_guard_task, "pairing_guard",
                                   kGuardTaskStackBytes, nullptr,
                                   kGuardTaskPriority, nullptr);
  if (created != pdPASS)
    ESP_LOGW(TAG, "Could not create pairing cleanup guard task");
}
