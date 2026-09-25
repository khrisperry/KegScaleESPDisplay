#include "app.h"
#include "ble_client.h"
#include "cJSON.h"
#include "controller_link.h"
#include "connection_transport.h"
#include "connection_session.h"
#include "controller_setup_client.h"
#include "setup_discovery.h"
#include "esp_app_desc.h"
#include "esp_event.h"
#include "esp_http_client.h"
#include "esp_heap_caps.h"
#include <algorithm>
#include <atomic>
#include <cstdint>
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_ota_ops.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_websocket_client.h"
#include "esp_wifi.h"
#include "freertos/task.h"
#include "mdns.h"
#include "nvs.h"
#include "nvs_flash.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <strings.h>

QueueHandle_t actions;

static void touchscreen_apply_settings_live();
static void touchscreen_ui_message(const char *message);
static bool touchscreen_remove_scale_pairing(const char *host);
static void *touchscreen_pairing_memset(void *dest, int value, size_t count);
static esp_err_t touchscreen_pairing_cl_open(cl_session_t *session,
                                             const uint8_t *in, size_t len,
                                             char *out);
extern "C" void touchscreen_request_auto_discovery();

namespace {
const char *TAG = "wifi_touchscreen";
Settings settings{};

// Scale 1 stays in the original Settings blob for migration safety. Scale 2
// is persisted separately so existing touchscreen configuration is preserved.
struct StoredScaleProfile {
  char host[128];
  uint8_t master[32];
  bool paired;
};
StoredScaleProfile secondary_scale{};
uint8_t active_scale_index = 0;

char *scale_host(uint8_t index) {
  return index == 0 ? settings.host : secondary_scale.host;
}
const char *scale_host_const(uint8_t index) {
  return index == 0 ? settings.host : secondary_scale.host;
}
bool &scale_paired(uint8_t index) {
  return index == 0 ? settings.paired : secondary_scale.paired;
}
uint8_t *scale_master(uint8_t index) {
  return index == 0 ? settings.master : secondary_scale.master;
}
char *active_host() { return scale_host(active_scale_index); }
const char *active_host_const() { return scale_host_const(active_scale_index); }
bool scale_slot_ready(uint8_t index) {
  return scale_host_const(index)[0] && scale_paired(index);
}
void publish_scale_profiles() {
  ui_scale_profiles(settings.host, settings.paired, secondary_scale.host,
                    secondary_scale.paired, active_scale_index);
}
std::atomic<bool> wifi_ready{false};

// Keep an unreachable saved scale from monopolizing the application loop.
// Paired scales can go directly to the asynchronous WebSocket client; the
// controller HTTP setup probe is only required while establishing pairing.
constexpr int64_t kReconnectRetryUs = 10000000LL;
constexpr int64_t kStateStaleUs = 12000000LL;

int scale_index_for_link(cl_session_t *session) {
  if (session == &connection_for(0).link)
    return 0;
  if (session == &connection_for(1).link)
    return 1;
  return -1;
}
int scale_index_for_master(void *master) {
  if (master == settings.master)
    return 0;
  if (master == secondary_scale.master)
    return 1;
  return -1;
}
void publish_active_state() { ui_state(connection_for(active_scale_index).state); }

const char *str(cJSON *o, const char *k) {
  auto v = cJSON_GetObjectItemCaseSensitive(o, k);
  return cJSON_IsString(v) ? v->valuestring : "";
}
double num(cJSON *o, const char *k) {
  auto v = cJSON_GetObjectItemCaseSensitive(o, k);
  return cJSON_IsNumber(v) ? v->valuedouble : 0;
}
bool yes(cJSON *o, const char *k) {
  return cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(o, k));
}
int64_t now() { return esp_timer_get_time(); }

esp_err_t persist() {
  nvs_handle_t n;
  esp_err_t e = nvs_open("touchscreen", NVS_READWRITE, &n);
  if (e != ESP_OK) {
    ESP_LOGE(TAG, "Could not open touchscreen NVS for write: %s",
             esp_err_to_name(e));
    return e;
  }
  e = nvs_set_blob(n, "settings", &settings, sizeof(settings));
  if (e == ESP_OK)
    e = nvs_set_blob(n, "scale2", &secondary_scale, sizeof(secondary_scale));
  if (e == ESP_OK)
    e = nvs_set_u8(n, "active_scale", active_scale_index);
  if (e == ESP_OK)
    e = nvs_commit(n);
  nvs_close(n);
  if (e != ESP_OK)
    ESP_LOGE(TAG, "Could not persist touchscreen settings: %s",
             esp_err_to_name(e));
  return e;
}
void wifi_event(void *, esp_event_base_t base, int32_t id, void *event_data) {
  if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
    ESP_LOGI(TAG, "Wi-Fi station started; configured SSID='%s'",
             settings.ssid[0] ? settings.ssid : "<none>");
  } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_CONNECTED) {
    ESP_LOGI(TAG, "Wi-Fi associated with SSID='%s'; waiting for DHCP",
             settings.ssid);
  } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
    wifi_ready = false;
    auto *d = static_cast<wifi_event_sta_disconnected_t *>(event_data);
    ESP_LOGW(TAG, "Wi-Fi disconnected from SSID='%s' reason=%u; reconnecting",
             settings.ssid, d ? (unsigned)d->reason : 0U);
    for (uint8_t slot = 0; slot < 2; ++slot) {
      auto &c = connection_for(slot);
      if (scale_host_const(slot)[0]) {
        // Paired scales stay connected in the background. An unpaired scale
        // only needs setup probing when it is the scale the user selected.
        c.retry_connection =
            scale_paired(slot) || slot == active_scale_index;
        c.next_connection_attempt =
            c.retry_connection ? now() + kReconnectRetryUs : 0;
      }
    }
    esp_err_t e = esp_wifi_connect();
    if (e != ESP_OK)
      ESP_LOGW(TAG, "Wi-Fi reconnect request failed: %s", esp_err_to_name(e));
  } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
    wifi_ready = true;
    ESP_LOGI(TAG, "Wi-Fi has an IP address; enabling both scale connections");
    for (uint8_t slot = 0; slot < 2; ++slot) {
      auto &c = connection_for(slot);
      if (scale_host_const(slot)[0]) {
        c.retry_connection =
            scale_paired(slot) || slot == active_scale_index;
        c.next_connection_attempt = c.retry_connection ? now() : 0;
      }
    }
  }
}

void disconnected(uint8_t slot) {
  auto &c = connection_for(slot);
  const SessionResetSnapshot reset = connection_session_reset(slot);
  if (slot == active_scale_index)
    ui_state(c.state);
  if (reset.was_authenticated || reset.was_online)
    ESP_LOGI(TAG,
             "Scale %u session reset: authenticated=%d online=%d",
             (unsigned)(slot + 1), reset.was_authenticated, reset.was_online);
  if (c.pending_id) {
    ESP_LOGW(TAG,
             "Connection lost with scale %u command id=%lu op='%s' pending",
             (unsigned)(slot + 1), (unsigned long)c.pending_id,
             c.pending_op);
    if (slot == active_scale_index)
      ui_result(false, c.pending_op,
                "Connection lost. Outcome unknown; check the scale before retrying.");
    c.pending_id = 0;
  }
}

void stop_scale_transport(uint8_t slot, bool retry) {
  auto &c = connection_for(slot);
  const uint32_t retired_generation = c.generation.fetch_add(1);
  auto old_ws = c.ws;
  c.ws = nullptr;

  if (old_ws &&
      !retire_transport_async(slot, old_ws, retired_generation)) {
    /*
     * Do not block the main/UI task trying to stop a wedged WebSocket. Restore
     * ownership of the handle so the next retry can queue it again instead of
     * leaking the transport.
     */
    c.ws = old_ws;
    c.retry_connection = retry && scale_host_const(slot)[0];
    c.next_connection_attempt = now() + kReconnectRetryUs;
    disconnected(slot);
    return;
  }

  disconnected(slot);
  c.retry_connection = retry && scale_host_const(slot)[0];
  c.next_connection_attempt = now() + kReconnectRetryUs;
}

bool send_secure(uint8_t slot, const char *plain) {
  return connection_session_send_secure(slot, plain, now());
}

bool scale_accepting_connection(uint8_t slot) {
  const char *host = scale_host_const(slot);
  ESP_LOGI(TAG, "Probing scale %u controller setup endpoint for host='%s'",
           (unsigned)(slot + 1), host);

  ControllerSetupResponse response{};
  const esp_err_t err = controller_setup_get(host, 2000, &response);

  ESP_LOGI(TAG,
           "Scale %u setup probe result: host='%s' open=%s headers=%lld status=%d bytes=%d",
           (unsigned)(slot + 1), host, esp_err_to_name(err),
           (long long)response.headers, response.http_status,
           response.bytes_received);
  if (response.bytes_received > 0)
    ESP_LOGI(TAG, "Scale %u setup response: %s",
             (unsigned)(slot + 1), response.body);

  if (response.http_status != 200 || response.bytes_received <= 0) {
    if (slot == active_scale_index)
      touchscreen_ui_message(
          response.http_status == 404
              ? "Update scale firmware to enable Wi-Fi touchscreen setup."
              : "Scale unreachable. Check its address and Wi-Fi network.");
    return false;
  }

  if (!response.json_valid || !response.has_paired) {
    ESP_LOGW(TAG, "Scale %u setup response was not expected controller JSON",
             (unsigned)(slot + 1));
    if (slot == active_scale_index)
      touchscreen_ui_message(
          "Address did not return scale setup. Check the scale address.");
    return false;
  }

  const bool paired = response.paired;
  const bool pending = response.pending;
  const double seconds = response.has_seconds ? response.seconds : 0;
  connection_for(slot).pairing_deadline_us =
      now() + (int64_t)(seconds * 1000000);
  const bool connected = response.connected;
  const bool ready = paired == scale_paired(slot) && (paired || seconds > 0);

  ESP_LOGI(TAG,
           "Scale %u controller status: scale_paired=%d display_paired=%d connected=%d pending=%d pairing_seconds=%.0f ready=%d",
           (unsigned)(slot + 1), paired, scale_paired(slot), connected,
           pending, seconds, ready);

  if (slot == active_scale_index) {
    if (paired && !scale_paired(slot))
      touchscreen_ui_message(
          "On scale: remove old Wi-Fi touchscreen, then Add touchscreen.");
    else if (!paired && scale_paired(slot))
      touchscreen_ui_message(
          "In Setup: Remove pairing, then Add touchscreen on the scale.");
    else if (!ready)
      touchscreen_ui_message(
          "On scale: Wi-Fi touchscreen setup > Add touchscreen.");
  }
  return ready;
}

void connect_scale(uint8_t slot) {
  auto &c = connection_for(slot);

  if (c.retirement_pending) {
    c.retry_connection = true;
    c.next_connection_attempt = now();
    ESP_LOGD(TAG,
             "Scale %u reconnect deferred until previous WebSocket retirement completes",
             (unsigned)(slot + 1));
    return;
  }

  const uint32_t generation = c.generation.fetch_add(1) + 1;
  c.retry_connection = true;
  c.next_connection_attempt = now() + kReconnectRetryUs;
  ESP_LOGI(TAG,
           "Scale %u connection attempt: host='%s' wifi_ready=%d display_paired=%d generation=%lu",
           (unsigned)(slot + 1),
           scale_host_const(slot)[0] ? scale_host_const(slot) : "<none>",
           (bool)wifi_ready, scale_paired(slot), (unsigned long)generation);

  if (c.ws) {
    auto old_ws = c.ws;
    c.ws = nullptr;
    ESP_LOGI(TAG,
             "Queueing previous scale %u WebSocket for background retirement; invalidated generation=%lu",
             (unsigned)(slot + 1), (unsigned long)(generation - 1));
    if (!retire_transport_async(slot, old_ws, generation - 1)) {
      c.ws = old_ws;
      c.retry_connection = true;
      c.next_connection_attempt = now() + kReconnectRetryUs;
      disconnected(slot);
      return;
    }
    disconnected(slot);
    c.retry_connection = true;
    c.next_connection_attempt = now();
    return;
  }
  disconnected(slot);

  if (!scale_host_const(slot)[0]) {
    c.retry_connection = false;
    ESP_LOGI(TAG, "Scale %u connection skipped: no hostname/IP saved",
             (unsigned)(slot + 1));
    return;
  }
  if (!wifi_ready) {
    ESP_LOGW(TAG, "Scale %u connection deferred: Wi-Fi has no IP yet",
             (unsigned)(slot + 1));
    if (slot == active_scale_index)
      touchscreen_ui_message("Waiting for Wi-Fi. Check SSID and password in Setup.");
    return;
  }
  if (!scale_paired(slot)) {
    if (slot != active_scale_index) {
      // Do not run the blocking HTTP setup probe for an unused/unpaired
      // secondary scale. The probe can spend seconds in DNS/HTTP timeouts,
      // starving the frame consumer and overflowing the Scale 1 frame queue.
      c.retry_connection = false;
      c.next_connection_attempt = 0;
      ESP_LOGI(TAG,
               "Scale %u is configured but inactive/unpaired; deferring setup probe until selected",
               (unsigned)(slot + 1));
      return;
    }
    if (!scale_accepting_connection(slot)) {
      ESP_LOGW(TAG,
               "Scale %u connection stopped before WebSocket: setup endpoint is not accepting this display",
               (unsigned)(slot + 1));
      c.next_connection_attempt = now() + kReconnectRetryUs;
      return;
    }
  } else {
    ESP_LOGD(TAG,
             "Scale %u has a saved pairing; skipping blocking HTTP setup probe",
             (unsigned)(slot + 1));
  }

  snprintf(c.uri, sizeof(c.uri), "ws://%s/ws/controller",
           scale_host_const(slot));
  ESP_LOGI(TAG,
           "Starting scale %u WebSocket connection: uri='%s' paired=%d generation=%lu",
           (unsigned)(slot + 1), c.uri, scale_paired(slot),
           (unsigned long)generation);
  esp_websocket_client_config_t config = {};
  config.uri = c.uri;
  config.buffer_size = CL_MAX_FRAME + 1;
  config.task_stack = 6144;
  config.disable_auto_reconnect = true;
  config.reconnect_timeout_ms = 5000;
  config.network_timeout_ms = 5000;
  /*
   * We already have protocol-level liveness in both directions:
   * - Touch sends an authenticated heartbeat every 8 seconds.
   * - Scale expires the session if it receives nothing for 15 seconds.
   * - Touch reconnects if Scale state is stale for more than 12 seconds.
   *
   * Do not let esp_websocket_client's independent PING/PONG timeout recycle a
   * healthy authenticated session. Control-frame PINGs may still be sent, but
   * application liveness remains authoritative.
   */
  config.disable_pingpong_discon = true;
  c.ws = esp_websocket_client_init(&config);
  if (!c.ws) {
    ESP_LOGE(TAG, "esp_websocket_client_init failed for scale %u %s",
             (unsigned)(slot + 1), c.uri);
    if (slot == active_scale_index)
      touchscreen_ui_message("Could not start scale WebSocket connection");
    return;
  }

  const uint32_t token = (generation << 1) | slot;
  esp_websocket_register_events(
      c.ws, WEBSOCKET_EVENT_ANY, socket_event,
      reinterpret_cast<void *>(static_cast<uintptr_t>(token)));
  esp_err_t e = esp_websocket_client_start(c.ws);
  c.retry_connection = e != ESP_OK;
  ESP_LOGI(TAG,
           "Scale %u WebSocket start returned %s; retry_connection=%d generation=%lu",
           (unsigned)(slot + 1), esp_err_to_name(e), c.retry_connection,
           (unsigned long)generation);
  if (e != ESP_OK && slot == active_scale_index)
    touchscreen_ui_message("Could not start scale WebSocket connection");
}

void on_frame(const Frame &f) {
  if (f.slot > 1)
    return;
  const uint8_t slot = f.slot;
  auto &c = connection_for(slot);
  if (f.generation != c.generation.load()) {
    ESP_LOGD(TAG,
             "Dropping queued stale scale %u frame generation=%lu current=%lu",
             (unsigned)(slot + 1), (unsigned long)f.generation,
             (unsigned long)c.generation.load());
    return;
  }

  if (f.kind == 2) {
    ESP_LOGW(TAG, "Processing scale %u WebSocket disconnect; retry in 10 seconds",
             (unsigned)(slot + 1));
    c.retry_connection = c.cancel_pairing_deadline_us == 0;
    c.cancel_pairing_deadline_us = 0;
    c.next_connection_attempt = now() + kReconnectRetryUs;
    disconnected(slot);
    if (slot == active_scale_index && !scale_paired(slot))
      touchscreen_pairing_ended();
    if (slot == active_scale_index)
      touchscreen_ui_message("Disconnected — reconnecting to scale");
    return;
  }

  if (f.kind == 1) {
    ESP_LOGI(TAG,
             "Scale %u WebSocket connected; beginning protocol-1 handshake",
             (unsigned)(slot + 1));
    disconnected(slot);

    char hello[320] = {};
    esp_err_t e = connection_session_prepare_hello(
        slot, scale_paired(slot), scale_master(slot),
        esp_app_get_description()->version, hello, sizeof(hello));
    if (e != ESP_OK) {
      ESP_LOGE(TAG, "Scale %u pairing key initialization failed: %s",
               (unsigned)(slot + 1), esp_err_to_name(e));
      if (slot == active_scale_index)
        touchscreen_ui_message("Pairing initialization failed");
      return;
    }

    if (scale_paired(slot))
      ESP_LOGI(TAG, "Sending hello for saved scale %u pairing",
               (unsigned)(slot + 1));
    else
      ESP_LOGI(TAG, "Sending hello for new scale %u pairing",
               (unsigned)(slot + 1));
    int sent = esp_websocket_client_send_text(
        c.ws, hello, strlen(hello), pdMS_TO_TICKS(1000));
    if (sent != (int)strlen(hello))
      ESP_LOGW(TAG,
               "Scale %u protocol hello send failed/incomplete: sent=%d expected=%u",
               (unsigned)(slot + 1), sent, (unsigned)strlen(hello));
    return;
  }

  char plain[CL_MAX_PLAIN];
  if (f.kind == 4) {
    esp_err_t e = touchscreen_pairing_cl_open(&c.link, f.bytes, f.length, plain);
    if (e != ESP_OK) {
      ESP_LOGW(TAG, "Scale %u encrypted frame authentication failed: %s",
               (unsigned)(slot + 1), esp_err_to_name(e));
      if (slot == active_scale_index)
        touchscreen_ui_message("Scale authentication failed");
      return;
    }
  } else {
    if (f.length >= sizeof(plain)) {
      ESP_LOGW(TAG, "Scale %u plain WebSocket frame too large: %u",
               (unsigned)(slot + 1), (unsigned)f.length);
      return;
    }
    memcpy(plain, f.bytes, f.length);
    plain[f.length] = 0;
  }

  cJSON *o = cJSON_Parse(plain);
  if (!o) {
    ESP_LOGW(TAG,
             "Scale %u WebSocket message is not valid JSON (kind=%d len=%u)",
             (unsigned)(slot + 1), f.kind, (unsigned)f.length);
    return;
  }
  const char *type = str(o, "type");

  if (f.kind == 3 && !c.authenticated &&
      (!strcmp(type, "pair") || !strcmp(type, "challenge"))) {
    ESP_LOGI(TAG, "Scale %u handshake message type='%s'",
             (unsigned)(slot + 1), type);
    if (num(o, "protocol") != 1) {
      ESP_LOGW(TAG, "Scale %u handshake validation failed: protocol=%.0f",
               (unsigned)(slot + 1), num(o, "protocol"));
      cJSON_Delete(o);
      return;
    }

    const bool pairing = !strcmp(type, "pair");
    const cJSON *seconds_field =
        cJSON_GetObjectItemCaseSensitive(o, "seconds");
    SessionHandshakeResult handshake{};
    esp_err_t e = connection_session_accept_handshake(
        slot, scale_paired(slot), pairing, str(o, "challenge"),
        str(o, "public"), now(), cJSON_IsNumber(seconds_field),
        num(o, "seconds"), &handshake);

    if (e == ESP_OK && handshake.pairing && slot == active_scale_index) {
      ESP_LOGI(TAG,
               "Scale %u key agreement succeeded; displaying approval code",
               (unsigned)(slot + 1));
      touchscreen_pairing_window(handshake.approval_seconds);
      ui_pair_code(handshake.approval_code);
    }

    if (e == ESP_OK && handshake.send_auth) {
      if (!send_secure(slot, "{\"type\":\"auth\"}"))
        ESP_LOGW(TAG, "Failed sending encrypted scale %u auth proof",
                 (unsigned)(slot + 1));
    }

    if (e == ESP_ERR_INVALID_STATE && !scale_paired(slot)) {
      ESP_LOGW(TAG,
               "Scale %u sent saved-pairing challenge but display has no saved pairing",
               (unsigned)(slot + 1));
    }
    if (e != ESP_OK && slot == active_scale_index)
      touchscreen_ui_message("Pairing failed. Reopen pairing on the scale.");
  } else if (f.kind == 4 && !strcmp(type, "pairing_canceled") && !scale_paired(slot)) {
    c.cancel_pairing_deadline_us = 0;
    stop_scale_transport(slot, false);
    if (slot == active_scale_index) {
      touchscreen_pairing_ended();
      touchscreen_ui_message("Pairing canceled. Start again from the Scale when ready.");
    }
  } else if (f.kind == 4 && !strcmp(type, "authorized")) {
    c.cancel_pairing_deadline_us = 0;
    ESP_LOGI(TAG, "Scale %u authorized this touchscreen",
             (unsigned)(slot + 1));
    if (!scale_paired(slot)) {
      memcpy(scale_master(slot), c.link.master, 32);
      scale_paired(slot) = true;
      esp_err_t e = persist();
      if (e != ESP_OK) {
        scale_paired(slot) = false;
        ESP_LOGE(TAG, "Could not save scale %u pairing: %s",
                 (unsigned)(slot + 1), esp_err_to_name(e));
        if (slot == active_scale_index)
          touchscreen_ui_message("Could not save pairing. Retry setup.");
        cJSON_Delete(o);
        return;
      }
      publish_scale_profiles();
      ESP_LOGI(TAG, "New scale %u pairing saved to NVS",
               (unsigned)(slot + 1));
    }
    c.authenticated = true;
    c.retry_connection = false;
    if (slot == active_scale_index) {
      ui_paired();
      touchscreen_ui_message("Connected to scale");
    }
    ESP_LOGI(TAG,
             "Scale %u session authenticated; internal_free=%u largest_internal=%u",
             (unsigned)(slot + 1),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL |
                                               MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(
                 MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
  } else if (f.kind == 4 && c.authenticated && !strcmp(type, "state")) {
    const bool first_state = !c.state.online;
    c.last_state = now();
    c.last_reading = c.last_state;
    c.state.age_seconds = 0;
    c.state.online = true;
    c.state.valid = yes(o, "reading_valid");
    c.state.ready = yes(o, "ready");
    c.state.stable = yes(o, "stable");
    c.state.has_tare = yes(o, "has_tare");
    c.state.calibrated = yes(o, "calibrated");
    c.state.revision = num(o, "revision");
    c.state.weight = num(o, "weight");
    c.state.gallons = num(o, "gallons");
    c.state.servings = num(o, "servings");
    c.state.percent = num(o, "percent");
    c.state.capacity = num(o, "capacity");
    c.state.empty = num(o, "empty");
    c.state.density = num(o, "density");
    c.state.serving = num(o, "serving");
    snprintf(c.state.name, sizeof(c.state.name), "%s", str(o, "name"));
    snprintf(c.state.firmware, sizeof(c.state.firmware), "%s",
             str(o, "firmware"));
    if (first_state)
      ESP_LOGI(TAG,
               "First scale %u state: firmware=%s ready=%d valid=%d stable=%d weight=%.3f lb revision=%lu",
               (unsigned)(slot + 1), c.state.firmware, c.state.ready,
               c.state.valid, c.state.stable, (double)c.state.weight,
               (unsigned long)c.state.revision);
    if (slot == active_scale_index)
      ui_state(c.state);
  } else if (f.kind == 4 && c.authenticated && !strcmp(type, "result") &&
             num(o, "id") == c.pending_id && c.pending_id) {
    bool result_ok = yes(o, "ok");
    const char *result_error = str(o, "error");
    char session_error[96] = {};

    if (!strcmp(c.pending_op, "begin_calibration") && result_ok) {
      const double session_value = num(o, "calibration_session_id");
      if (!std::isfinite(session_value) || session_value < 1 ||
          session_value > UINT32_MAX || floor(session_value) != session_value) {
        result_ok = false;
        snprintf(session_error, sizeof(session_error),
                 "Scale did not return a valid calibration session. Start again.");
        result_error = session_error;
        c.calibration_session_id = 0;
      } else {
        c.calibration_session_id = (uint32_t)session_value;
      }
    } else if (result_ok &&
               (!strcmp(c.pending_op, "calibrate") ||
                !strcmp(c.pending_op, "cancel_calibration"))) {
      c.calibration_session_id = 0;
    } else if (!result_ok && c.calibration_session_id &&
               (!strcmp(c.pending_op, "tare") ||
                !strcmp(c.pending_op, "calibrate") ||
                !strcmp(c.pending_op, "cancel_calibration")) &&
               strstr(result_error, "session expired")) {
      c.calibration_session_id = 0;
    }

    ESP_LOGI(TAG,
             "Scale %u command result: id=%lu op='%s' ok=%d calibration_session=%lu",
             (unsigned)(slot + 1), (unsigned long)c.pending_id,
             c.pending_op, result_ok,
             (unsigned long)c.calibration_session_id);
    if (slot == active_scale_index)
      ui_result(result_ok, c.pending_op, result_error);
    c.pending_id = 0;
  } else {
    ESP_LOGD(TAG,
             "Ignoring scale %u message type='%s' kind=%d authenticated=%d pending_id=%lu",
             (unsigned)(slot + 1), type, f.kind, c.authenticated,
             (unsigned long)c.pending_id);
  }
  cJSON_Delete(o);
}

void normalize_host(const char *host, char *buffer, size_t size) {
  if (!buffer || size == 0)
    return;
  buffer[0] = 0;
  if (!host || !host[0])
    return;

  const char *start = host;
  if (!strncmp(start, "http://", 7))
    start += 7;
  else if (!strncmp(start, "https://", 8))
    start += 8;

  size_t used = 0;
  while (start[used] && start[used] != '/' && start[used] != ':' &&
         used + 1 < size) {
    buffer[used] = start[used];
    ++used;
  }
  buffer[used] = 0;
}

bool peer_matches_host(const ble_client_peer_t &peer, const char *host) {
  char normalized[128];
  normalize_host(host, normalized, sizeof(normalized));
  if (!normalized[0])
    return false;

  size_t len = strlen(normalized);
  if (len > 6 && !strcasecmp(normalized + len - 6, ".local"))
    normalized[len - 6] = 0;

  if (peer.scale_id[0] && !strcasecmp(peer.scale_id, normalized))
    return true;

  return peer.setup_url_available && peer.ip_address[0] &&
         !strcmp(peer.ip_address, normalized);
}

bool host_is_other_slot(const char *host, uint8_t slot) {
  const uint8_t other = slot == 0 ? 1 : 0;
  const char *other_host = scale_host_const(other);
  return host && host[0] && other_host && other_host[0] &&
         !strcasecmp(host, other_host);
}

void send_web_discovery_result(const char *scale_name, const char *host) {
  char url[192];
  snprintf(url, sizeof(url), "http://%s/", host);
  touchscreen_home_discovery_result(TOUCHSCREEN_DISCOVERY_WEB,
                                    scale_name, url, host);
}

void discover_unpaired_scale_qr(uint8_t slot) {
  if (slot > 1)
    slot = 0;

  auto &slot_connection = connection_for(slot);
  if (scale_paired(slot) || slot_connection.authenticated ||
      slot_connection.state.online) {
    ESP_LOGI(TAG,
             "Skipping disconnected QR discovery for scale %u: paired/authenticated/online",
             (unsigned)(slot + 1));
    touchscreen_home_discovery_result(TOUCHSCREEN_DISCOVERY_NONE,
                                      "", "", "");
    return;
  }

  const char *saved = scale_host_const(slot);
  char saved_match[140] = {};
  char only_network_host[140] = {};
  char only_network_name[64] = {};
  unsigned eligible_network_count = 0;
  bool network_was_ambiguous = false;

  if (wifi_ready.load()) {
    ESP_LOGI(TAG,
             "Disconnected QR: checking mDNS for unpaired scale %u",
             (unsigned)(slot + 1));

    mdns_result_t *found = nullptr;
    esp_err_t mdns_err =
        mdns_query_ptr("_kegscale", "_tcp", 1800, 8, &found);

    if (mdns_err == ESP_OK) {
      for (auto p = found; p; p = p->next) {
        if (!p->hostname || !p->hostname[0])
          continue;

        char host[140];
        snprintf(host, sizeof(host), "%s.local", p->hostname);

        if (host_is_other_slot(host, slot))
          continue;

        ++eligible_network_count;
        if (eligible_network_count == 1) {
          snprintf(only_network_host, sizeof(only_network_host), "%s", host);
          snprintf(only_network_name, sizeof(only_network_name), "%s",
                   p->hostname);
        }

        if (saved && saved[0] && !strcasecmp(saved, host))
          snprintf(saved_match, sizeof(saved_match), "%s", host);
      }

      network_was_ambiguous =
          eligible_network_count > 1 && !saved_match[0];

      if (found)
        mdns_query_results_free(found);

      if (saved_match[0]) {
        ESP_LOGI(TAG, "Disconnected QR: mDNS matched saved scale '%s'",
                 saved_match);
        send_web_discovery_result(saved_match, saved_match);
        return;
      }

      if (eligible_network_count == 1) {
        ESP_LOGI(TAG, "Disconnected QR: one mDNS scale found '%s'",
                 only_network_host);
        send_web_discovery_result(
            only_network_name[0] ? only_network_name : only_network_host,
            only_network_host);
        return;
      }
    } else {
      ESP_LOGW(TAG, "Disconnected QR mDNS scan failed: %s",
               esp_err_to_name(mdns_err));
    }
  }

  /*
   * mDNS can take long enough for a connection/pairing transition to finish.
   * Re-check immediately before bringing Bluetooth up so a stale queued
   * discover_qr action can never start BLE on an already-connected display.
   */
  if (scale_paired(slot) || connection_for(slot).authenticated ||
      connection_for(slot).state.online) {
    ESP_LOGI(TAG,
             "Disconnected QR: scale %u became connected before BLE fallback; BLE will remain off",
             (unsigned)(slot + 1));
    touchscreen_home_discovery_result(TOUCHSCREEN_DISCOVERY_NONE,
                                      "", "", "");
    return;
  }

  ESP_LOGI(TAG,
           "Disconnected QR: starting temporary BLE discovery for unpaired scale %u",
           (unsigned)(slot + 1));

  esp_err_t ble_err = ble_client_init();
  if (ble_err != ESP_OK) {
    ESP_LOGW(TAG, "Disconnected QR BLE init failed: %s",
             esp_err_to_name(ble_err));
    touchscreen_home_discovery_result(
        network_was_ambiguous ? TOUCHSCREEN_DISCOVERY_MULTIPLE
                              : TOUCHSCREEN_DISCOVERY_ERROR,
        "", "",
        network_was_ambiguous
            ? "Several network scales were found. Open Setup to choose one."
            : "Bluetooth discovery could not start. Open Setup and try Find scale.");
    return;
  }

  ble_client_peer_t peers[BLE_CLIENT_MAX_CANDIDATES] = {};
  size_t peer_count = 0;
  ble_err = ble_client_scan(peers, BLE_CLIENT_MAX_CANDIDATES,
                            &peer_count, 2500);

  /*
   * BLE is only a discovery fallback for the touchscreen. Do not leave
   * NimBLE/controller resources alive while the normal Wi-Fi controller
   * connection is running.
   */
  const esp_err_t ble_shutdown_err = ble_client_deinit();
  if (ble_shutdown_err == ESP_OK) {
    ESP_LOGI(TAG,
             "Disconnected QR: BLE discovery finished and Bluetooth is off; internal_free=%u largest_internal=%u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL |
                                               MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(
                 MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
  } else {
    ESP_LOGW(TAG,
             "Disconnected QR: BLE shutdown failed: %s",
             esp_err_to_name(ble_shutdown_err));
  }

  if (ble_err != ESP_OK) {
    ESP_LOGW(TAG, "Disconnected QR BLE scan failed: %s",
             esp_err_to_name(ble_err));
    touchscreen_home_discovery_result(
        network_was_ambiguous ? TOUCHSCREEN_DISCOVERY_MULTIPLE
                              : TOUCHSCREEN_DISCOVERY_ERROR,
        "", "",
        network_was_ambiguous
            ? "Several network scales were found. Open Setup to choose one."
            : "Bluetooth scan failed. Open Setup and try Find scale.");
    return;
  }

  const uint8_t other = slot == 0 ? 1 : 0;
  const char *other_host = scale_host_const(other);
  int selected = -1;
  int saved_selected = -1;
  unsigned eligible = 0;

  for (size_t i = 0; i < peer_count; ++i) {
    if (!peers[i].scale_id[0])
      continue;

    if (other_host && other_host[0] &&
        peer_matches_host(peers[i], other_host))
      continue;

    ++eligible;
    selected = (int)i;

    if (saved && saved[0] && peer_matches_host(peers[i], saved))
      saved_selected = (int)i;
  }

  if (saved_selected >= 0)
    selected = saved_selected;
  else if (eligible != 1)
    selected = -1;

  if (selected < 0) {
    if (eligible > 1 || network_was_ambiguous) {
      ESP_LOGI(TAG,
               "Disconnected QR: multiple unpaired scale candidates found");
      touchscreen_home_discovery_result(
          TOUCHSCREEN_DISCOVERY_MULTIPLE, "", "",
          "Several scales are nearby. Open Setup to choose the correct one.");
    } else {
      ESP_LOGI(TAG, "Disconnected QR: no scale candidate found");
      touchscreen_home_discovery_result(
          TOUCHSCREEN_DISCOVERY_NOT_FOUND, "", "",
          "No scale was found on Wi-Fi or Bluetooth.");
    }
    return;
  }

  const ble_client_peer_t &peer = peers[selected];

  if (peer.setup_url_available && peer.ip_address[0]) {
    ESP_LOGI(TAG,
             "Disconnected QR: BLE found %s at LAN IP %s",
             peer.scale_id, peer.ip_address);
    send_web_discovery_result(peer.scale_id, peer.ip_address);
    return;
  }

  static const char prefix[] = "KegScale-";
  const char *suffix =
      !strncasecmp(peer.scale_id, prefix, sizeof(prefix) - 1)
          ? peer.scale_id + sizeof(prefix) - 1
          : peer.scale_id;

  char setup_ssid[48];
  snprintf(setup_ssid, sizeof(setup_ssid),
           "KegScale-Setup-%s", suffix);

  char wifi_qr[128];
  snprintf(wifi_qr, sizeof(wifi_qr),
           "WIFI:T:nopass;S:%s;;", setup_ssid);

  char detail[150];
  snprintf(detail, sizeof(detail),
           "%s - the captive setup page should open automatically. "
           "If it does not, open 192.168.4.1.",
           setup_ssid);

  ESP_LOGI(TAG,
           "Disconnected QR: BLE found offline scale %s; setup AP '%s'",
           peer.scale_id, setup_ssid);

  touchscreen_home_discovery_result(
      TOUCHSCREEN_DISCOVERY_SETUP_WIFI,
      peer.scale_id, wifi_qr, detail);
}

void action(const Action &a) {
  cJSON *o = cJSON_Parse(a.body);

  if (!strcmp(a.kind, "cancel_pairing")) {
    const uint8_t slot = active_scale_index;
    auto &c = connection_for(slot);
    // Approval may have arrived while Cancel was queued. Never erase that key.
    if (!scale_paired(slot)) {
      const bool sent = c.traffic_ready && send_secure(slot, "{\"type\":\"cancel_pairing\"}");
      if (sent) {
        // Wait for the encrypted outcome: cancellation or a concurrent approval.
        c.cancel_pairing_deadline_us = now() + 3000000;
      } else {
        stop_scale_transport(slot, false);
        touchscreen_pairing_ended();
        touchscreen_ui_message("Pairing stopped here. The Scale window will expire automatically.");
      }
    } else {
      touchscreen_pairing_ended();
      touchscreen_ui_message("Pairing already completed; saved connection kept.");
    }
    cJSON_Delete(o);
    return;
  }

  if (!strcmp(a.kind, "pairing_rearm") && o) {
    const int requested_slot = (int)num(o, "slot");
    const uint8_t slot = requested_slot == 1 ? 1 : 0;

    if (scale_paired(slot)) {
      ESP_LOGI(TAG,
               "Ignoring Scale %u pairing rearm because pairing is already saved",
               (unsigned)(slot + 1));
    } else if (!scale_host_const(slot)[0]) {
      ESP_LOGW(TAG,
               "Ignoring Scale %u pairing rearm because no host is configured",
               (unsigned)(slot + 1));
    } else if (slot != active_scale_index) {
      /*
       * Never steal the UI or transport from the selected Scale. The Add
       * touchscreen window remains open on the inactive Scale; the user can
       * select that slot in Setup and Save & connect when ready.
       */
      ESP_LOGI(TAG,
               "Scale %u pairing window is ready while Scale %u remains active; leaving active connection untouched",
               (unsigned)(slot + 1), (unsigned)(active_scale_index + 1));
      char message[96];
      snprintf(message, sizeof(message),
               "Scale %u is ready to pair. Select it in Setup and Save & connect.",
               (unsigned)(slot + 1));
      touchscreen_ui_message(message);
    } else {
      auto &c = connection_for(slot);
      ESP_LOGI(TAG,
               "Re-arming Scale %u pairing connection without resetting Wi-Fi or the other Scale",
               (unsigned)(slot + 1));
      c.retry_connection = true;
      c.next_connection_attempt = now();
      touchscreen_ui_message("Pairing window found - connecting to scale...");
      connect_scale(slot);
    }

    cJSON_Delete(o);
    return;
  }

  if (!strcmp(a.kind, "settings") && o) {
    const char *host = str(o, "host");
    const int requested_slot = (int)num(o, "slot");
    const uint8_t slot = requested_slot == 1 ? 1 : 0;
    bool valid = strlen(host) < 128;
    for (const char *p = host; *p; ++p)
      if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
            (*p >= '0' && *p <= '9') || *p == '.' || *p == '-'))
        valid = false;

    const uint8_t other = slot == 0 ? 1 : 0;
    const bool duplicate_host =
        host[0] && scale_host_const(other)[0] &&
        !strcmp(host, scale_host_const(other));

    if (!valid || strlen(str(o, "ssid")) > 32 ||
        strlen(str(o, "password")) > 64) {
      ESP_LOGW(TAG,
               "Rejected settings update: invalid host/SSID/password length");
      touchscreen_ui_message("Enter a hostname or IPv4 address without http:// or a port");
    } else if (duplicate_host) {
      ESP_LOGW(TAG,
               "Rejected scale %u settings: host '%s' is already assigned to scale %u",
               (unsigned)(slot + 1), host, (unsigned)(other + 1));
      touchscreen_ui_message("That scale is already assigned to the other slot.");
    } else {
      const Settings previous_settings = settings;
      const StoredScaleProfile previous_secondary = secondary_scale;
      const uint8_t previous_active = active_scale_index;
      active_scale_index = slot;

      const bool host_changed = strcmp(host, scale_host_const(slot)) != 0;
      ESP_LOGI(TAG,
               "Saving connection settings: slot=%u ssid='%s' host='%s' host_changed=%d previous_paired=%d brightness=%.0f",
               (unsigned)(slot + 1), str(o, "ssid"), host, host_changed,
               scale_paired(slot), num(o, "brightness"));

      if (host_changed) {
        scale_paired(slot) = false;
        touchscreen_pairing_memset(scale_master(slot), 0, 32);
        snprintf(scale_host(slot), 128, "%s", host);
        stop_scale_transport(slot, false);
      }

      snprintf(settings.ssid, sizeof(settings.ssid), "%s", str(o, "ssid"));
      snprintf(settings.password, sizeof(settings.password), "%s",
               str(o, "password"));
      settings.brightness = (uint8_t)num(o, "brightness");

      if (persist() == ESP_OK) {
        publish_scale_profiles();
        publish_active_state();
        ESP_LOGI(TAG, "Settings saved; applying touchscreen configuration");
        touchscreen_ui_message("Settings saved — restarting");
        vTaskDelay(pdMS_TO_TICKS(100));
        touchscreen_apply_settings_live();
      } else {
        settings = previous_settings;
        secondary_scale = previous_secondary;
        active_scale_index = previous_active;
        auto &restore_connection = connection_for(slot);
        restore_connection.retry_connection = scale_host_const(slot)[0];
        restore_connection.next_connection_attempt = now();
        publish_scale_profiles();
        publish_active_state();
        touchscreen_ui_message("Could not save settings");
      }
    }
  } else if (!strcmp(a.kind, "discover_qr")) {
    discover_unpaired_scale_qr(active_scale_index);
  } else if (!strcmp(a.kind, "scan_wifi")) {
    if (!setup_discovery_start_wifi_scan(a.ui_generation))
      ui_message_for_generation(
          "Wi-Fi scan is already running. Please wait.", a.ui_generation);
  } else if (!strcmp(a.kind, "discover")) {
    if (!setup_discovery_start_scale_scan(a.ui_generation))
      ui_message_for_generation(
          "Scale discovery is already running. Please wait.", a.ui_generation);
  } else if (!strcmp(a.kind, "forget")) {
    const int requested_slot = o ? (int)num(o, "slot") : active_scale_index;
    const uint8_t slot = requested_slot == 1 ? 1 : 0;
    const uint8_t other = slot == 0 ? 1 : 0;
    const bool removed_active = slot == active_scale_index;
    ESP_LOGI(TAG, "Removing saved touchscreen-side pairing for scale %u",
             (unsigned)(slot + 1));
    scale_paired(slot) = false;
    touchscreen_pairing_memset(scale_master(slot), 0, 32);
    stop_scale_transport(slot, false);
    if (removed_active && scale_slot_ready(other))
      active_scale_index = other;
    if (persist() == ESP_OK) {
      publish_scale_profiles();
      publish_active_state();
      touchscreen_ui_message("Pairing removed. Use Add touchscreen to pair this scale again.");
    } else {
      touchscreen_ui_message("Could not remove pairing");
    }
  } else if (!strcmp(a.kind, "switch_scale")) {
    auto &current = connection_for(active_scale_index);
    if (!scale_slot_ready(0) || !scale_slot_ready(1)) {
      touchscreen_ui_message("Configure and pair both scales before switching.");
    } else if (current.pending_id) {
      touchscreen_ui_message("Wait for the current scale operation to finish");
    } else if (current.calibration_session_id) {
      touchscreen_ui_message("Cancel calibration before switching scales.");
    } else {
      const uint8_t previous = active_scale_index;
      active_scale_index = active_scale_index == 0 ? 1 : 0;
      ESP_LOGI(TAG,
               "Switching display from scale %u to scale %u host='%s' cached_online=%d",
               (unsigned)(previous + 1), (unsigned)(active_scale_index + 1),
               active_host_const(),
               connection_for(active_scale_index).state.online);
      if (persist() != ESP_OK) {
        active_scale_index = previous;
        touchscreen_ui_message("Could not save active scale selection");
      } else {
        publish_scale_profiles();
        publish_active_state();
        auto &selected = connection_for(active_scale_index);
        if (!selected.state.online) {
          selected.retry_connection = true;
          selected.next_connection_attempt = now();
          touchscreen_ui_message(active_scale_index == 0
                         ? "Scale 1 selected — reconnecting..."
                         : "Scale 2 selected — reconnecting...");
        } else {
          touchscreen_ui_message(active_scale_index == 0 ? "Scale 1 selected"
                                             : "Scale 2 selected");
        }
      }
    }
  } else if (!strcmp(a.kind, "ota")) {
    auto &current = connection_for(active_scale_index);
    if (current.pending_id) {
      touchscreen_ui_message("Wait for the current operation to finish");
    } else {
      ESP_LOGI(TAG, "Starting touchscreen OTA check without dropping scale sessions");
      esp_err_t e = touchscreen_ota_request(false, true);
      ESP_LOGI(TAG, "Touchscreen OTA returned %s", esp_err_to_name(e));
      if (e != ESP_OK)
        touchscreen_ui_message(esp_err_to_name(e));
    }
  } else if (!strcmp(a.kind, "command") && o) {
    const uint8_t slot = active_scale_index;
    auto &c = connection_for(slot);
    const char *op = str(o, "op");
    const bool calibration_followup =
        !strcmp(op, "tare") ||
        !strcmp(op, "calibrate") ||
        !strcmp(op, "cancel_calibration");

    if (!c.authenticated || !c.state.online) {
      ESP_LOGW(TAG,
               "Cannot send scale %u command '%s': authenticated=%d online=%d",
               (unsigned)(slot + 1), op, c.authenticated,
               c.state.online);
      ui_result(false, op,
                "Scale disconnected. Reconnect before making changes.");
    } else if (c.pending_id) {
      touchscreen_ui_message("Wait for the previous operation to finish");
    } else if (calibration_followup && !c.calibration_session_id) {
      ui_result(false, op,
                "Calibration session is no longer active. Start calibration again.");
    } else {
      if (calibration_followup) {
        cJSON_AddNumberToObject(
            o, "calibration_session_id", c.calibration_session_id);
      }
      cJSON_AddStringToObject(o, "type", "command");
      cJSON_AddNumberToObject(o, "id", ++c.request_id);
      char *plain = cJSON_PrintUnformatted(o);
      snprintf(c.pending_op, sizeof(c.pending_op), "%s", op);
      c.pending_id = c.request_id;
      c.pending_since = now();
      ESP_LOGI(TAG, "Sending scale %u command: id=%lu op='%s'",
               (unsigned)(slot + 1), (unsigned long)c.pending_id,
               c.pending_op);
      if (!plain || !send_secure(slot, plain)) {
        ESP_LOGW(TAG, "Scale %u command send failed: id=%lu op='%s'",
                 (unsigned)(slot + 1), (unsigned long)c.pending_id,
                 c.pending_op);
        ui_result(false, c.pending_op,
                  "Send failed. Check the scale before retrying.");
        c.pending_id = 0;
      }
      free(plain);
    }
  }

  cJSON_Delete(o);
}
} // namespace

// Runtime services formerly layered through main_wrapper.cpp. They now use
// explicit hooks from main.cpp so the application compiles as a normal source
// file without source inclusion or macro interception.
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
constexpr uint32_t kOtaReconnectDeferMs = 2000;
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

/*
 * HTTPS OTA and a reconnecting WebSocket both consume internal networking
 * resources. When the active scale has a saved pairing, give reconnect
 * priority and do not start TLS until that saved session is authenticated and
 * delivering state again.
 *
 * An unconfigured or unpaired touchscreen is not blocked here; OTA still
 * remains available during initial setup. A live unpaired pairing WebSocket is
 * treated as busy so OTA does not compete with the approval handshake.
 */
bool ota_scale_transport_busy() {
  if (!wifi_ready.load())
    return true;

  const uint8_t slot = active_scale_index;
  if (!scale_host_const(slot)[0])
    return false;

  const auto &c = connection_for_const(slot);

  if (c.pending_id)
    return true;

  if (scale_paired(slot))
    return !c.authenticated || !c.state.online;

  return c.ws && esp_websocket_client_is_connected(c.ws);
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

  bool reconnect_wait_logged = false;
  for (;;) {
    if (!wifi_ready) {
      vTaskDelay(pdMS_TO_TICKS(1000));
      continue;
    }

    if (ota_scale_transport_busy()) {
      if (!reconnect_wait_logged) {
        const auto &c = connection_for_const(active_scale_index);
        ESP_LOGI(TAG,
                 "Automatic OTA check deferred until scale %u reconnect settles "
                 "(paired=%d authenticated=%d online=%d)",
                 (unsigned)(active_scale_index + 1),
                 scale_paired(active_scale_index), c.authenticated,
                 c.state.online);
        reconnect_wait_logged = true;
      }
      vTaskDelay(pdMS_TO_TICKS(kOtaReconnectDeferMs));
      continue;
    }

    if (reconnect_wait_logged) {
      ESP_LOGI(TAG,
               "Scale connection settled; automatic OTA check may proceed");
      reconnect_wait_logged = false;
    }

    const bool install = ota_auto_install.load();
    esp_err_t e = touchscreen_ota_request(install, false);
    if (e == ESP_ERR_INVALID_STATE)
      ESP_LOGI(TAG,
               "Automatic OTA check deferred because networking/update work is busy");
    else if (e != ESP_OK)
      ESP_LOGW(TAG, "Could not schedule automatic OTA check: %s",
               esp_err_to_name(e));

    /*
     * Only start the normal daily interval once we have actually attempted
     * this cycle. If the scale was reconnecting above, we stay in the short
     * defer loop rather than losing the check for 24 hours.
     */
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
          ? setup_discovery_build_scale_options(found, options, sizeof(options))
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

  int status = 0;
  const esp_err_t e = controller_setup_remove(host, 3000, &status);
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
  if (ota_scale_transport_busy()) {
    const auto &c = connection_for_const(active_scale_index);
    ESP_LOGI(TAG,
             "Touchscreen OTA %s deferred: scale %u connection is busy "
             "(paired=%d authenticated=%d online=%d)",
             install ? "install" : "check",
             (unsigned)(active_scale_index + 1),
             scale_paired(active_scale_index), c.authenticated,
             c.state.online);
    if (foreground || install)
      ui_update_error(
          "Scale is reconnecting. Wait for it to connect, then try the update again.",
          install);
    return ESP_ERR_INVALID_STATE;
  }

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

    // "Save & connect" is also an explicit reconnect request. If the selected
    // scale is configured but the session is not fully authenticated/online,
    // restart the connection even when SSID and hostname did not change.
    //
    // This is especially important after the full-screen pairing code times
    // out: the old WebSocket can still be sitting in an unfinished handshake.
    // connect_scale() increments the generation, tears down that stale socket,
    // probes the scale setup window again, and starts a fresh handshake.
    auto &c = connection_for(slot);
    const bool has_scale = scale_host_const(slot)[0] != 0;
    const bool needs_connection =
        has_scale &&
        (!scale_paired(slot) || !c.authenticated || !c.state.online);

    if (needs_connection) {
      ESP_LOGI(
          TAG,
          "Save & connect requested for scale %u with unchanged settings; "
          "restarting connection (paired=%d authenticated=%d online=%d)",
          (unsigned)(slot + 1), scale_paired(slot), c.authenticated,
          c.state.online);
      c.retry_connection = true;
      c.next_connection_attempt = now();
      touchscreen_ui_message(scale_paired(slot)
                                 ? "Reconnecting to scale..."
                                 : "Starting scale pairing...");
      connect_scale(slot);
    } else {
      touchscreen_ui_message("Settings saved");
    }
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

extern "C" void touchscreen_app_main() {
  ESP_LOGI(TAG, "Starting Wi-Fi touchscreen firmware %s",
           esp_app_get_description()->version);
#ifdef CONFIG_ESP_WS_CLIENT_SEPARATE_TX_LOCK
  ESP_LOGI(TAG, "WebSocket separate TX lock enabled");
#else
  ESP_LOGW(TAG, "WebSocket separate TX lock is NOT enabled; regenerate sdkconfig from defaults");
#endif
  ESP_LOGI(TAG, "Reset reason=%d", (int)esp_reset_reason());
  esp_err_t e = nvs_flash_init();
  if (e == ESP_ERR_NVS_NO_FREE_PAGES || e == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_LOGW(TAG, "NVS requires erase/reinitialize: %s", esp_err_to_name(e));
    ESP_ERROR_CHECK(nvs_flash_erase());
    e = nvs_flash_init();
  }
  ESP_ERROR_CHECK(e);
  settings.brightness = 85;
  nvs_handle_t n;
  if (nvs_open("touchscreen", NVS_READONLY, &n) == ESP_OK) {
    size_t size = sizeof(settings);
    if (nvs_get_blob(n, "settings", &settings, &size) != ESP_OK ||
        size != sizeof(settings)) {
      settings = {};
      settings.brightness = 85;
      ESP_LOGI(TAG, "No valid saved touchscreen settings; starting unconfigured");
    }
    size_t scale2_size = sizeof(secondary_scale);
    if (nvs_get_blob(n, "scale2", &secondary_scale, &scale2_size) != ESP_OK ||
        scale2_size != sizeof(secondary_scale))
      secondary_scale = {};
    uint8_t saved_active = 0;
    if (nvs_get_u8(n, "active_scale", &saved_active) == ESP_OK && saved_active < 2)
      active_scale_index = saved_active;
    nvs_close(n);
  }
  settings.ssid[32] = 0;
  settings.password[64] = 0;
  settings.host[127] = 0;
  secondary_scale.host[127] = 0;
  if (active_scale_index > 1)
    active_scale_index = 0;
  if (!scale_host_const(active_scale_index)[0] &&
      scale_host_const(active_scale_index == 0 ? 1 : 0)[0])
    active_scale_index = active_scale_index == 0 ? 1 : 0;
  ESP_LOGI(TAG,
           "Loaded settings: ssid='%s' scale1='%s' paired1=%d scale2='%s' paired2=%d active=%u brightness=%u",
           settings.ssid[0] ? settings.ssid : "<none>",
           settings.host[0] ? settings.host : "<none>", settings.paired,
           secondary_scale.host[0] ? secondary_scale.host : "<none>",
           secondary_scale.paired, (unsigned)(active_scale_index + 1),
           settings.brightness);
  actions = xQueueCreate(4, sizeof(Action));
  configASSERT(actions);
  connection_transport_init();
  ESP_ERROR_CHECK(psa_crypto_init() == PSA_SUCCESS ? ESP_OK : ESP_FAIL);
  publish_scale_profiles();
  ui_start(settings);
  ESP_ERROR_CHECK(esp_netif_init());
  ESP_ERROR_CHECK(esp_event_loop_create_default());
  auto netif = esp_netif_create_default_wifi_sta();
  uint8_t mac[6];
  esp_read_mac(mac, ESP_MAC_WIFI_STA);
  char hostname[32];
  snprintf(hostname, sizeof(hostname), "KegTouch-%02X%02X", mac[4], mac[5]);
  esp_netif_set_hostname(netif, hostname);
  ESP_LOGI(TAG, "Touchscreen network hostname: %s", hostname);
  wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
  ESP_ERROR_CHECK(esp_wifi_init(&cfg));
  ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                             wifi_event, nullptr));
  ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                             wifi_event, nullptr));
  ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
  ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
  wifi_config_t wifi = {};
  memcpy(wifi.sta.ssid, settings.ssid, strlen(settings.ssid));
  memcpy(wifi.sta.password, settings.password, strlen(settings.password));
  ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi));
  ESP_ERROR_CHECK(esp_wifi_start());
  esp_wifi_set_ps(WIFI_PS_NONE);
  if (settings.ssid[0]) {
    ESP_LOGI(TAG, "Requesting Wi-Fi connection to SSID='%s'", settings.ssid);
    e = esp_wifi_connect();
    if (e != ESP_OK)
      ESP_LOGW(TAG, "Initial Wi-Fi connect request failed: %s", esp_err_to_name(e));
  } else {
    ESP_LOGW(TAG, "No Wi-Fi SSID saved; connection cannot start");
  }
  e = mdns_init();
  if (e == ESP_OK) {
    mdns_hostname_set(hostname);
    ESP_LOGI(TAG, "mDNS initialized with hostname %s.local", hostname);
  } else {
    ESP_LOGW(TAG, "mDNS initialization failed: %s", esp_err_to_name(e));
  }
  for (uint8_t slot = 0; slot < 2; ++slot)
    connect_scale(slot);
  /* Hardware/UI, NVS and networking initialized. An offline scale must not
   * cause otherwise healthy display firmware to roll back. */
  esp_ota_mark_app_valid_cancel_rollback();
  Frame f;
  Action a;
  for (;;) {
    /*
     * Process a bounded frame batch, then give UI/actions a turn. A broken
     * Scale must not be able to starve Switch Scale or Setup actions by
     * continuously refilling the shared frame queue.
     */
    for (unsigned processed = 0; processed < 8; ++processed) {
      if (!connection_receive_frame(&f, 0))
        break;
      on_frame(f);
    }

    if (xQueueReceive(actions, &a, pdMS_TO_TICKS(30)) == pdTRUE)
      action(a);

    const int64_t current_time = now();
    for (uint8_t slot = 0; slot < 2; ++slot) {
      auto &c = connection_for(slot);

      if (c.cancel_pairing_deadline_us && current_time >= c.cancel_pairing_deadline_us) {
        c.cancel_pairing_deadline_us = 0;
        if (!scale_paired(slot)) {
          stop_scale_transport(slot, false);
          if (slot == active_scale_index) {
            touchscreen_pairing_ended();
            touchscreen_ui_message("Pairing stopped here. Check the Scale before retrying.");
          }
        }
      }

      if (c.retry_connection && scale_host_const(slot)[0] &&
          !c.retirement_pending &&
          current_time >= c.next_connection_attempt) {
        connect_scale(slot);
        continue;
      }

      if (c.ws && esp_websocket_client_is_connected(c.ws) &&
          c.traffic_ready &&
          current_time - c.last_ping > 8000000) {
        c.last_ping = current_time;
        if (!send_secure(slot, "{\"type\":\"ping\"}"))
          ESP_LOGW(TAG,
                   "Scale %u heartbeat send failed; reconnect scheduled",
                   (unsigned)(slot + 1));
      }

      if (!c.state.online && c.last_reading &&
          current_time - c.last_age_update > 1000000) {
        c.last_age_update = current_time;
        c.state.age_seconds =
            (current_time - c.last_reading) / 1000000;
        if (slot == active_scale_index)
          ui_state(c.state);
      }

      if (c.state.online && current_time - c.last_state > kStateStaleUs) {
        ESP_LOGW(TAG,
                 "Scale %u reading stale for >12 seconds; reconnecting",
                 (unsigned)(slot + 1));
        c.state.online = false;
        if (slot == active_scale_index) {
          ui_state(c.state);
          touchscreen_ui_message("Reading is stale — reconnecting");
        }
        connect_scale(slot);
        continue;
      }

      if (c.pending_id &&
          current_time - c.pending_since > 30000000) {
        ESP_LOGW(TAG,
                 "Scale %u command timed out: id=%lu op='%s'",
                 (unsigned)(slot + 1), (unsigned long)c.pending_id,
                 c.pending_op);
        if (slot == active_scale_index)
          ui_result(false, c.pending_op,
                    "No confirmation. Check the scale before retrying.");
        c.pending_id = 0;
        connect_scale(slot);
      }
    }
  }
}