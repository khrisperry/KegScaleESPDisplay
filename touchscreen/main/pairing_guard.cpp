#include "app.h"
#include "controller_setup_client.h"
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
constexpr unsigned kMaxPairWindowPolls = 300;

struct StoredScaleProfile {
  char host[128];
  uint8_t master[32];
  bool paired;
};

struct SlotState {
  char host[128];
  bool paired;
};

enum class CleanupPhase {
  Idle,
  VerifyRemoval,
  WaitForPairWindow,
};

struct PendingCleanup {
  CleanupPhase phase = CleanupPhase::Idle;
  char host[128];
  unsigned cleanup_attempts;
  unsigned pair_window_polls;
};

struct RemoteControllerStatus {
  bool paired;
  bool connected;
  bool pending;
  double seconds;
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

bool read_remote_status(const char *host, RemoteControllerStatus *status) {
  if (!host || !host[0] || !status)
    return false;

  ControllerSetupResponse response{};
  const esp_err_t e = controller_setup_get(host, 1500, &response);
  if (e != ESP_OK || response.http_status != 200 ||
      response.bytes_received <= 0 || !response.json_valid ||
      !response.has_paired || !response.has_seconds)
    return false;

  status->paired = response.paired;
  status->connected = response.connected;
  status->pending = response.pending;
  status->seconds = response.seconds;
  return true;
}

bool remove_remote_pairing(const char *host) {
  if (!host || !host[0])
    return false;

  int http_status = 0;
  const esp_err_t e =
      controller_setup_remove(host, 2000, &http_status);
  ESP_LOGI(TAG, "Cleanup POST host='%s' result=%s status=%d", host,
           esp_err_to_name(e), http_status);
  return e == ESP_OK && http_status == 200;
}

void start_cleanup(PendingCleanup *cleanup, const char *host, unsigned slot) {
  if (!cleanup || !host || !host[0])
    return;
  *cleanup = {};
  cleanup->phase = CleanupPhase::VerifyRemoval;
  snprintf(cleanup->host, sizeof(cleanup->host), "%s", host);
  ESP_LOGI(TAG,
           "Scale %u local pairing cleared; verifying scale-side cleanup for '%s'",
           slot + 1, cleanup->host);
}

void wait_for_pair_window(PendingCleanup *cleanup, unsigned slot) {
  cleanup->phase = CleanupPhase::WaitForPairWindow;
  cleanup->pair_window_polls = 0;
  ESP_LOGI(TAG,
           "Scale %u pairing is clear on both sides; waiting for Add touchscreen window",
           slot + 1);
}

bool rearm_connection_for_pairing(unsigned slot, const char *host) {
  ESP_LOGI(TAG,
           "Scale %u Add touchscreen window detected for '%s'; queueing slot-specific pairing reconnect",
           slot + 1, host);

  Action action{};
  snprintf(action.kind, sizeof(action.kind), "pairing_rearm");
  snprintf(action.body, sizeof(action.body), "{\"slot\":%u}", slot);
  if (xQueueSend(actions, &action, 0) != pdTRUE) {
    ESP_LOGW(TAG,
             "Could not queue Scale %u pairing reconnect; will retry without disturbing Wi-Fi",
             slot + 1);
    return false;
  }
  return true;
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

      if (cleanup[slot].phase != CleanupPhase::Idle && current[slot].paired &&
          !strcmp(current[slot].host, cleanup[slot].host)) {
        ESP_LOGI(TAG,
                 "Scale %u paired again; canceling stale cleanup/watch state",
                 slot + 1);
        cleanup[slot] = {};
      }

      previous[slot] = current[slot];
    }

    for (unsigned slot = 0; slot < 2; ++slot) {
      PendingCleanup &pending = cleanup[slot];
      if (pending.phase == CleanupPhase::Idle)
        continue;

      RemoteControllerStatus remote{};
      if (!read_remote_status(pending.host, &remote)) {
        if (pending.phase == CleanupPhase::VerifyRemoval) {
          ++pending.cleanup_attempts;
          if (pending.cleanup_attempts == 1 ||
              pending.cleanup_attempts % 10 == 0) {
            ESP_LOGW(TAG,
                     "Scale %u cleanup verification attempt %u could not reach '%s'",
                     slot + 1, pending.cleanup_attempts, pending.host);
          }
          if (pending.cleanup_attempts >= kMaxCleanupAttempts) {
            ESP_LOGW(TAG,
                     "Scale %u pairing cleanup gave up after %u attempts for '%s'",
                     slot + 1, pending.cleanup_attempts, pending.host);
            pending = {};
          }
        } else {
          ++pending.pair_window_polls;
          if (pending.pair_window_polls >= kMaxPairWindowPolls) {
            ESP_LOGI(TAG,
                     "Scale %u Add touchscreen watch expired for '%s'",
                     slot + 1, pending.host);
            pending = {};
          }
        }
        continue;
      }

      if (pending.phase == CleanupPhase::VerifyRemoval) {
        if (!remote.paired) {
          ESP_LOGI(TAG,
                   "Scale %u pairing cleanup verified complete after %u attempt(s)",
                   slot + 1, pending.cleanup_attempts + 1);
          wait_for_pair_window(&pending, slot);
          continue;
        }

        ++pending.cleanup_attempts;
        ESP_LOGW(TAG,
                 "Scale %u still reports an old pairing; removing it (attempt %u)",
                 slot + 1, pending.cleanup_attempts);
        if (remove_remote_pairing(pending.host)) {
          ESP_LOGI(TAG, "Scale %u stale pairing removed", slot + 1);
          wait_for_pair_window(&pending, slot);
        } else if (pending.cleanup_attempts >= kMaxCleanupAttempts) {
          ESP_LOGW(TAG,
                   "Scale %u pairing cleanup gave up after %u attempts for '%s'",
                   slot + 1, pending.cleanup_attempts, pending.host);
          pending = {};
        }
        continue;
      }

      ++pending.pair_window_polls;

      if (remote.paired) {
        ESP_LOGW(TAG,
                 "Scale %u regained a pairing while display is unpaired; clearing stale scale state",
                 slot + 1);
        pending.phase = CleanupPhase::VerifyRemoval;
        pending.cleanup_attempts = 0;
        if (remove_remote_pairing(pending.host))
          wait_for_pair_window(&pending, slot);
        continue;
      }

      if (remote.seconds > 0) {
        if (rearm_connection_for_pairing(slot, pending.host))
          pending = {};
        continue;
      }

      if (pending.pair_window_polls >= kMaxPairWindowPolls) {
        ESP_LOGI(TAG,
                 "Scale %u Add touchscreen watch expired for '%s'",
                 slot + 1, pending.host);
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
