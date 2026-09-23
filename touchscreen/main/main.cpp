#include "app.h"
#include "ble_client.h"
#include "cJSON.h"
#include "controller_link.h"
#include "esp_app_desc.h"
#include "esp_event.h"
#include "esp_http_client.h"
#include "esp_heap_caps.h"
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
#include "mdns.h"
#include "nvs.h"
#include "nvs_flash.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <strings.h>

QueueHandle_t actions;
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
struct Frame {
  uint8_t slot;
  uint32_t generation;
  int kind;
  size_t length;
  uint8_t bytes[CL_MAX_FRAME + 1];
};

struct ScaleConnection {
  cl_session_t link{};
  esp_websocket_client_handle_t ws = nullptr;
  Frame assembly{};
  char own_public[131] = {};
  char uri[180] = {};
  char pending_op[32] = {};
  uint8_t client_nonce[32] = {};
  uint32_t request_id = 0;
  uint32_t pending_id = 0;
  int64_t last_state = 0;
  int64_t pending_since = 0;
  int64_t last_ping = 0;
  int64_t last_reading = 0;
  int64_t last_age_update = 0;
  int64_t next_connection_attempt = 0;
  int64_t pairing_deadline_us = 0;
  int64_t cancel_pairing_deadline_us = 0;
  bool authenticated = false;
  bool traffic_ready = false;
  bool retry_connection = true;
  std::atomic<uint32_t> generation{0};
  State state{};
};

ScaleConnection connections[2];
QueueHandle_t frames;
std::atomic<bool> wifi_ready{false};

// Keep an unreachable saved scale from monopolizing the application loop.
// Paired scales can go directly to the asynchronous WebSocket client; the
// controller HTTP setup probe is only required while establishing pairing.
constexpr int64_t kReconnectRetryUs = 10000000LL;
constexpr int64_t kStateStaleUs = 12000000LL;

ScaleConnection &connection_for(uint8_t slot) {
  return connections[slot == 1 ? 1 : 0];
}
const ScaleConnection &connection_for_const(uint8_t slot) {
  return connections[slot == 1 ? 1 : 0];
}
int scale_index_for_link(cl_session_t *session) {
  if (session == &connections[0].link)
    return 0;
  if (session == &connections[1].link)
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

void socket_event(void *arg, esp_event_base_t, int32_t event, void *data) {
  const uint32_t token =
      static_cast<uint32_t>(reinterpret_cast<uintptr_t>(arg));
  const uint8_t slot = token & 1U;
  const uint32_t event_generation = token >> 1;
  if (slot > 1)
    return;
  auto &c = connection_for(slot);
  const uint32_t current_generation = c.generation.load();
  if (event_generation != current_generation) {
    ESP_LOGD(TAG,
             "Ignoring stale scale %u WebSocket event=%ld generation=%lu current=%lu",
             (unsigned)(slot + 1), (long)event,
             (unsigned long)event_generation,
             (unsigned long)current_generation);
    return;
  }

  Frame f{};
  f.slot = slot;
  f.generation = event_generation;
  if (event == WEBSOCKET_EVENT_CONNECTED) {
    ESP_LOGI(TAG, "Scale %u WebSocket transport connected to %s generation=%lu",
             (unsigned)(slot + 1), c.uri,
             (unsigned long)event_generation);
    f.kind = 1;
    if (xQueueSend(frames, &f, 0) != pdTRUE)
      ESP_LOGW(TAG, "Frame queue full while reporting scale %u connection",
               (unsigned)(slot + 1));
  } else if (event == WEBSOCKET_EVENT_DISCONNECTED) {
    ESP_LOGW(TAG,
             "Scale %u WebSocket transport disconnected from %s generation=%lu",
             (unsigned)(slot + 1), c.uri,
             (unsigned long)event_generation);
    f.kind = 2;
    if (xQueueSend(frames, &f, 0) != pdTRUE)
      ESP_LOGW(TAG, "Frame queue full while reporting scale %u disconnect",
               (unsigned)(slot + 1));
    c.assembly = {};
  } else if (event == WEBSOCKET_EVENT_ERROR) {
    ESP_LOGW(TAG,
             "Scale %u WebSocket transport error while connecting to %s generation=%lu",
             (unsigned)(slot + 1), c.uri,
             (unsigned long)event_generation);
  } else if (event == WEBSOCKET_EVENT_DATA) {
    auto *d = (esp_websocket_event_data_t *)data;
    if (d->op_code != 1 && d->op_code != 2)
      return;
    if (d->payload_offset == 0) {
      c.assembly = {};
      c.assembly.slot = slot;
      c.assembly.generation = event_generation;
      c.assembly.kind = d->op_code == 1 ? 3 : 4;
    }
    if (d->payload_len > CL_MAX_FRAME || d->payload_offset < 0 ||
        d->data_len < 0 || (size_t)d->payload_offset != c.assembly.length ||
        c.assembly.length + d->data_len > CL_MAX_FRAME) {
      ESP_LOGW(TAG,
               "Rejected scale %u WebSocket fragment: opcode=%d payload_len=%d offset=%d data_len=%d assembled=%u",
               (unsigned)(slot + 1), d->op_code, d->payload_len,
               d->payload_offset, d->data_len, (unsigned)c.assembly.length);
      return;
    }
    memcpy(c.assembly.bytes + c.assembly.length, d->data_ptr, d->data_len);
    c.assembly.length += d->data_len;
    if (c.assembly.length == (size_t)d->payload_len) {
      ESP_LOGD(TAG,
               "Complete scale %u WebSocket message received: opcode=%d len=%u",
               (unsigned)(slot + 1), d->op_code,
               (unsigned)c.assembly.length);
      if (xQueueSend(frames, &c.assembly, 0) != pdTRUE)
        ESP_LOGW(TAG, "Frame queue full; dropping scale %u WebSocket message",
                 (unsigned)(slot + 1));
    }
  }
}

void disconnected(uint8_t slot) {
  auto &c = connection_for(slot);
  const bool was_authenticated = c.authenticated;
  const bool was_online = c.state.online;
  c.authenticated = false;
  c.traffic_ready = false;
  c.state.online = false;
  c.last_state = 0;
  cl_clear(&c.link);
  if (slot == active_scale_index)
    ui_state(c.state);
  if (was_authenticated || was_online)
    ESP_LOGI(TAG,
             "Scale %u session reset: authenticated=%d online=%d",
             (unsigned)(slot + 1), was_authenticated, was_online);
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
  c.generation.fetch_add(1);
  if (c.ws) {
    esp_websocket_client_stop(c.ws);
    esp_websocket_client_destroy(c.ws);
    c.ws = nullptr;
  }
  disconnected(slot);
  c.retry_connection = retry && scale_host_const(slot)[0];
  c.next_connection_attempt = now() + kReconnectRetryUs;
}

bool send_secure(uint8_t slot, const char *plain) {
  auto &c = connection_for(slot);
  uint8_t out[CL_MAX_FRAME];
  size_t len = 0;
  esp_err_t e = cl_seal(&c.link, plain, out, &len);
  if (e != ESP_OK) {
    ESP_LOGW(TAG, "Could not encrypt scale %u WebSocket message: %s",
             (unsigned)(slot + 1), esp_err_to_name(e));
    return false;
  }
  if (!c.ws || !esp_websocket_client_is_connected(c.ws))
    return false;
  int sent = esp_websocket_client_send_bin(c.ws, (char *)out, len,
                                           pdMS_TO_TICKS(1000));
  if (sent != (int)len) {
    ESP_LOGW(TAG,
             "Scale %u encrypted WebSocket send incomplete: sent=%d expected=%u",
             (unsigned)(slot + 1), sent, (unsigned)len);
    return false;
  }
  return true;
}

bool scale_accepting_connection(uint8_t slot) {
  const char *host = scale_host_const(slot);
  char url[180], body[512] = {};
  snprintf(url, sizeof(url), "http://%s/api/controller", host);
  ESP_LOGI(TAG, "Probing scale %u setup endpoint: %s",
           (unsigned)(slot + 1), url);
  esp_http_client_config_t cfg = {};
  cfg.url = url;
  cfg.timeout_ms = 2000;
  auto client = esp_http_client_init(&cfg);
  if (!client) {
    ESP_LOGE(TAG, "Could not allocate HTTP client for scale %u probe",
             (unsigned)(slot + 1));
    return false;
  }
  esp_err_t err = esp_http_client_open(client, 0);
  int status = 0, received = -1;
  int64_t headers = -1;
  if (err == ESP_OK) {
    headers = esp_http_client_fetch_headers(client);
    if (headers >= 0) {
      status = esp_http_client_get_status_code(client);
      received = esp_http_client_read_response(client, body, sizeof(body) - 1);
    }
  }
  ESP_LOGI(TAG,
           "Scale %u setup probe result: host='%s' open=%s headers=%lld status=%d bytes=%d",
           (unsigned)(slot + 1), host, esp_err_to_name(err),
           (long long)headers, status, received);
  if (received > 0)
    ESP_LOGI(TAG, "Scale %u setup response: %s",
             (unsigned)(slot + 1), body);
  esp_http_client_close(client);
  esp_http_client_cleanup(client);

  if (status != 200 || received <= 0) {
    if (slot == active_scale_index)
      ui_message(status == 404
                     ? "Update scale firmware to enable Wi-Fi touchscreen setup."
                     : "Scale unreachable. Check its address and Wi-Fi network.");
    return false;
  }

  auto o = cJSON_Parse(body);
  if (!o || !cJSON_IsBool(cJSON_GetObjectItemCaseSensitive(o, "paired"))) {
    ESP_LOGW(TAG, "Scale %u setup response was not expected controller JSON",
             (unsigned)(slot + 1));
    cJSON_Delete(o);
    if (slot == active_scale_index)
      ui_message("Address did not return scale setup. Check the scale address.");
    return false;
  }

  const bool paired = yes(o, "paired");
  const bool pending = yes(o, "pending");
  const double seconds = num(o, "seconds");
  connection_for(slot).pairing_deadline_us = now() + (int64_t)(seconds * 1000000);
  const bool connected = yes(o, "connected");
  const bool ready = paired == scale_paired(slot) && (paired || seconds > 0);
  ESP_LOGI(TAG,
           "Scale %u controller status: scale_paired=%d display_paired=%d connected=%d pending=%d pairing_seconds=%.0f ready=%d",
           (unsigned)(slot + 1), paired, scale_paired(slot), connected,
           pending, seconds, ready);
  if (slot == active_scale_index) {
    if (paired && !scale_paired(slot))
      ui_message("On scale: remove old Wi-Fi touchscreen, then Add touchscreen.");
    else if (!paired && scale_paired(slot))
      ui_message("In Setup: Remove pairing, then Add touchscreen on the scale.");
    else if (!ready)
      ui_message("On scale: Wi-Fi touchscreen setup > Add touchscreen.");
  }
  cJSON_Delete(o);
  return ready;
}

void connect_scale(uint8_t slot) {
  auto &c = connection_for(slot);
  const uint32_t generation = c.generation.fetch_add(1) + 1;
  c.retry_connection = true;
  c.next_connection_attempt = now() + kReconnectRetryUs;
  ESP_LOGI(TAG,
           "Scale %u connection attempt: host='%s' wifi_ready=%d display_paired=%d generation=%lu",
           (unsigned)(slot + 1),
           scale_host_const(slot)[0] ? scale_host_const(slot) : "<none>",
           (bool)wifi_ready, scale_paired(slot), (unsigned long)generation);

  if (c.ws) {
    ESP_LOGI(TAG,
             "Stopping previous scale %u WebSocket before reconnect; invalidated generation=%lu",
             (unsigned)(slot + 1), (unsigned long)(generation - 1));
    esp_websocket_client_stop(c.ws);
    esp_websocket_client_destroy(c.ws);
    c.ws = nullptr;
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
      ui_message("Waiting for Wi-Fi. Check SSID and password in Setup.");
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
  c.ws = esp_websocket_client_init(&config);
  if (!c.ws) {
    ESP_LOGE(TAG, "esp_websocket_client_init failed for scale %u %s",
             (unsigned)(slot + 1), c.uri);
    if (slot == active_scale_index)
      ui_message("Could not start scale WebSocket connection");
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
    ui_message("Could not start scale WebSocket connection");
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
      ui_message("Disconnected — reconnecting to scale");
    return;
  }

  if (f.kind == 1) {
    ESP_LOGI(TAG,
             "Scale %u WebSocket connected; beginning protocol-1 handshake",
             (unsigned)(slot + 1));
    disconnected(slot);
    char hello[320], nonce_hex[65];
    esp_fill_random(c.client_nonce, 32);
    cl_hex(c.client_nonce, 32, nonce_hex);
    if (scale_paired(slot)) {
      memcpy(c.link.master, scale_master(slot), 32);
      snprintf(hello, sizeof(hello),
               "{\"type\":\"hello\",\"protocol\":1,\"nonce\":\"%s\"}",
               nonce_hex);
      ESP_LOGI(TAG, "Sending hello for saved scale %u pairing",
               (unsigned)(slot + 1));
    } else {
      esp_err_t e = cl_keypair(&c.link, c.own_public);
      if (e != ESP_OK) {
        ESP_LOGE(TAG, "Scale %u pairing key initialization failed: %s",
                 (unsigned)(slot + 1), esp_err_to_name(e));
        if (slot == active_scale_index)
          ui_message("Pairing initialization failed");
        return;
      }
      snprintf(hello, sizeof(hello),
               "{\"type\":\"hello\",\"protocol\":1,\"public\":\"%s\",\"nonce\":\"%s\"}",
               c.own_public, nonce_hex);
      ESP_LOGI(TAG, "Sending hello for new scale %u pairing",
               (unsigned)(slot + 1));
    }
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
    esp_err_t e = cl_open(&c.link, f.bytes, f.length, plain);
    if (e != ESP_OK) {
      ESP_LOGW(TAG, "Scale %u encrypted frame authentication failed: %s",
               (unsigned)(slot + 1), esp_err_to_name(e));
      if (slot == active_scale_index)
        ui_message("Scale authentication failed");
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
    uint8_t challenge[32];
    if (num(o, "protocol") != 1 ||
        !cl_unhex(str(o, "challenge"), challenge, 32)) {
      ESP_LOGW(TAG, "Scale %u handshake validation failed: protocol=%.0f",
               (unsigned)(slot + 1), num(o, "protocol"));
      cJSON_Delete(o);
      return;
    }

    const bool pairing = !strcmp(type, "pair");
    esp_err_t e = ESP_OK;
    if (pairing) {
      char code[13];
      e = cl_agree(&c.link, str(o, "public"), str(o, "public"),
                   c.own_public, code);
      if (e == ESP_OK && slot == active_scale_index) {
        ESP_LOGI(TAG,
                 "Scale %u key agreement succeeded; displaying approval code",
                 (unsigned)(slot + 1));
        // New Scales include the actual remaining window. For older Scales,
        // retain the deadline obtained by the setup probe instead of starting
        // an unrelated timer when the code arrives.
        const cJSON *seconds_field = cJSON_GetObjectItemCaseSensitive(o, "seconds");
        const int64_t remaining = c.pairing_deadline_us - now();
        const uint32_t seconds = cJSON_IsNumber(seconds_field)
            ? (uint32_t)std::max(0.0, std::min(300.0, num(o, "seconds")))
            : (remaining > 0 ? (uint32_t)((remaining + 999999) / 1000000) : 0);
        touchscreen_pairing_window(seconds);
        ui_pair_code(code);
      }
    } else if (!scale_paired(slot)) {
      ESP_LOGW(TAG,
               "Scale %u sent saved-pairing challenge but display has no saved pairing",
               (unsigned)(slot + 1));
      e = ESP_FAIL;
    }

    if (e == ESP_OK) {
      e = cl_start(&c.link, challenge, c.client_nonce, false);
      c.traffic_ready = e == ESP_OK;
      ESP_LOGI(TAG, "Scale %u traffic-key setup returned %s",
               (unsigned)(slot + 1), esp_err_to_name(e));
    }
    if (e == ESP_OK && !pairing) {
      if (!send_secure(slot, "{\"type\":\"auth\"}"))
        ESP_LOGW(TAG, "Failed sending encrypted scale %u auth proof",
                 (unsigned)(slot + 1));
    }
    if (e != ESP_OK && slot == active_scale_index)
      ui_message("Pairing failed. Reopen pairing on the scale.");
  } else if (f.kind == 4 && !strcmp(type, "pairing_canceled") && !scale_paired(slot)) {
    c.cancel_pairing_deadline_us = 0;
    stop_scale_transport(slot, false);
    if (slot == active_scale_index) {
      touchscreen_pairing_ended();
      ui_message("Pairing canceled. Start again from the Scale when ready.");
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
          ui_message("Could not save pairing. Retry setup.");
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
      ui_message("Connected to scale");
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
    ESP_LOGI(TAG,
             "Scale %u command result: id=%lu op='%s' ok=%d",
             (unsigned)(slot + 1), (unsigned long)c.pending_id,
             c.pending_op, yes(o, "ok"));
    if (slot == active_scale_index)
      ui_result(yes(o, "ok"), c.pending_op, str(o, "error"));
    c.pending_id = 0;
  } else {
    ESP_LOGD(TAG,
             "Ignoring scale %u message type='%s' kind=%d authenticated=%d pending_id=%lu",
             (unsigned)(slot + 1), type, f.kind, c.authenticated,
             (unsigned long)c.pending_id);
  }
  cJSON_Delete(o);
}

unsigned build_scale_discovery_options(mdns_result_t *found, char *options,
                                       size_t options_size) {
  if (!options || options_size == 0)
    return 0;
  options[0] = 0;
  const char *manual = "Manual IP / hostname...";
  const size_t manual_reserve = strlen(manual) + 2;
  unsigned count = 0;

  for (auto p = found; p; p = p->next) {
    if (!p->hostname || !p->hostname[0])
      continue;
    char host[140];
    snprintf(host, sizeof(host), "%s.local", p->hostname);
    bool duplicate = false;
    const char *scan = options;
    const size_t host_len = strlen(host);
    while (*scan) {
      const char *line_end = strchr(scan, '\n');
      const size_t line_len =
          line_end ? (size_t)(line_end - scan) : strlen(scan);
      if (line_len == host_len && !strncmp(scan, host, host_len)) {
        duplicate = true;
        break;
      }
      if (!line_end)
        break;
      scan = line_end + 1;
    }
    if (duplicate)
      continue;

    const size_t used = strlen(options);
    const size_t append_len = host_len + (used ? 1 : 0);
    if (used + append_len + manual_reserve >= options_size)
      break;
    if (used)
      strcat(options, "\n");
    strcat(options, host);
    ++count;
    ESP_LOGI(TAG, "mDNS scale candidate: hostname='%s' port=%u",
             p->hostname, p->port);
  }

  if (options[0])
    strcat(options, "\n");
  strcat(options, manual);
  return count;
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
        ui_message("Pairing stopped here. The Scale window will expire automatically.");
      }
    } else {
      touchscreen_pairing_ended();
      ui_message("Pairing already completed; saved connection kept.");
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
      ui_message("Enter a hostname or IPv4 address without http:// or a port");
    } else if (duplicate_host) {
      ESP_LOGW(TAG,
               "Rejected scale %u settings: host '%s' is already assigned to scale %u",
               (unsigned)(slot + 1), host, (unsigned)(other + 1));
      ui_message("That scale is already assigned to the other slot.");
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
        memset(scale_master(slot), 0, 32);
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
        ui_message("Settings saved — restarting");
        vTaskDelay(pdMS_TO_TICKS(100));
        esp_restart();
      } else {
        settings = previous_settings;
        secondary_scale = previous_secondary;
        active_scale_index = previous_active;
        auto &restore_connection = connection_for(slot);
        restore_connection.retry_connection = scale_host_const(slot)[0];
        restore_connection.next_connection_attempt = now();
        publish_scale_profiles();
        publish_active_state();
        ui_message("Could not save settings");
      }
    }
  } else if (!strcmp(a.kind, "discover_qr")) {
    discover_unpaired_scale_qr(active_scale_index);
  } else if (!strcmp(a.kind, "scan_wifi")) {
    ESP_LOGI(TAG, "Starting Wi-Fi network scan");
    wifi_scan_config_t scan = {};
    esp_err_t scan_err = esp_wifi_scan_start(&scan, true);
    if (scan_err == ESP_OK) {
      wifi_ap_record_t aps[20];
      uint16_t count = 20;
      esp_wifi_scan_get_ap_records(&count, aps);
      ESP_LOGI(TAG, "Wi-Fi scan completed: %u access points returned", count);
      char options[700] = "";
      for (unsigned i = 0; i < count; ++i) {
        const char *ssid = (char *)aps[i].ssid;
        if (!ssid[0] || strchr(ssid, '\n'))
          continue;
        if (options[0])
          strcat(options, "\n");
        strncat(options, ssid, 32);
      }
      ui_networks(options[0] ? options : "No networks found");
    } else {
      ESP_LOGW(TAG, "Wi-Fi scan failed: %s", esp_err_to_name(scan_err));
      ui_message("Wi-Fi scan failed. Enter the SSID manually.");
    }
  } else if (!strcmp(a.kind, "discover")) {
    ESP_LOGI(TAG, "Starting mDNS discovery for _kegscale._tcp");
    mdns_result_t *found = nullptr;
    esp_err_t e = mdns_query_ptr("_kegscale", "_tcp", 3000, 8, &found);
    char options[800];
    const unsigned count =
        e == ESP_OK ? build_scale_discovery_options(found, options,
                                                    sizeof(options))
                    : 0;
    if (e != ESP_OK)
      snprintf(options, sizeof(options), "Manual IP / hostname...");
    ESP_LOGI(TAG, "mDNS discovery completed: result=%s candidates=%u",
             esp_err_to_name(e), count);
    ui_discovered_options(options);
    if (count > 1)
      ui_message("Several scales found. Choose one from the list.");
    else if (count == 1)
      ui_message("Scale found. Choose it or use Manual IP entry.");
    else
      ui_message("No scale found. Choose Manual IP / hostname.");
    if (found)
      mdns_query_results_free(found);
  } else if (!strcmp(a.kind, "forget")) {
    const int requested_slot = o ? (int)num(o, "slot") : active_scale_index;
    const uint8_t slot = requested_slot == 1 ? 1 : 0;
    const uint8_t other = slot == 0 ? 1 : 0;
    const bool removed_active = slot == active_scale_index;
    ESP_LOGI(TAG, "Removing saved touchscreen-side pairing for scale %u",
             (unsigned)(slot + 1));
    scale_paired(slot) = false;
    memset(scale_master(slot), 0, 32);
    stop_scale_transport(slot, false);
    if (removed_active && scale_slot_ready(other))
      active_scale_index = other;
    if (persist() == ESP_OK) {
      publish_scale_profiles();
      publish_active_state();
      ui_message("Pairing removed. Use Add touchscreen to pair this scale again.");
    } else {
      ui_message("Could not remove pairing");
    }
  } else if (!strcmp(a.kind, "switch_scale")) {
    auto &current = connection_for(active_scale_index);
    if (!scale_slot_ready(0) || !scale_slot_ready(1)) {
      ui_message("Configure and pair both scales before switching.");
    } else if (current.pending_id) {
      ui_message("Wait for the current scale operation to finish");
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
        ui_message("Could not save active scale selection");
      } else {
        publish_scale_profiles();
        publish_active_state();
        auto &selected = connection_for(active_scale_index);
        if (!selected.state.online) {
          selected.retry_connection = true;
          selected.next_connection_attempt = now();
          ui_message(active_scale_index == 0
                         ? "Scale 1 selected — reconnecting..."
                         : "Scale 2 selected — reconnecting...");
        } else {
          ui_message(active_scale_index == 0 ? "Scale 1 selected"
                                             : "Scale 2 selected");
        }
      }
    }
  } else if (!strcmp(a.kind, "ota")) {
    auto &current = connection_for(active_scale_index);
    if (current.pending_id) {
      ui_message("Wait for the current operation to finish");
    } else {
      ESP_LOGI(TAG, "Starting touchscreen OTA check without dropping scale sessions");
      esp_err_t e = touchscreen_ota(false);
      ESP_LOGI(TAG, "Touchscreen OTA returned %s", esp_err_to_name(e));
      if (e != ESP_OK)
        ui_message(esp_err_to_name(e));
    }
  } else if (!strcmp(a.kind, "command") && o) {
    const uint8_t slot = active_scale_index;
    auto &c = connection_for(slot);
    if (!c.authenticated || !c.state.online) {
      ESP_LOGW(TAG,
               "Cannot send scale %u command '%s': authenticated=%d online=%d",
               (unsigned)(slot + 1), str(o, "op"), c.authenticated,
               c.state.online);
      ui_result(false, str(o, "op"),
                "Scale disconnected. Reconnect before making changes.");
    } else if (c.pending_id) {
      ui_message("Wait for the previous operation to finish");
    } else {
      cJSON_AddStringToObject(o, "type", "command");
      cJSON_AddNumberToObject(o, "id", ++c.request_id);
      char *plain = cJSON_PrintUnformatted(o);
      snprintf(c.pending_op, sizeof(c.pending_op), "%s", str(o, "op"));
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
extern "C" void app_main() {
  ESP_LOGI(TAG, "Starting Wi-Fi touchscreen firmware %s",
           esp_app_get_description()->version);
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
  frames = xQueueCreate(8, sizeof(Frame));
  configASSERT(actions && frames);
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
    while (xQueueReceive(frames, &f, 0) == pdTRUE)
      on_frame(f);

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
            ui_message("Pairing stopped here. Check the Scale before retrying.");
          }
        }
      }

      if (c.retry_connection && scale_host_const(slot)[0] &&
          current_time >= c.next_connection_attempt) {
        connect_scale(slot);
        continue;
      }

      if (c.ws && esp_websocket_client_is_connected(c.ws) &&
          current_time - c.last_ping > 3000000) {
        c.last_ping = current_time;
        if (c.traffic_ready &&
            !send_secure(slot, "{\"type\":\"ping\"}"))
          ESP_LOGW(TAG, "Scale %u heartbeat send failed",
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
          ui_message("Reading is stale — reconnecting");
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
