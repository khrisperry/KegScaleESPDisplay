#include "app.h"
#include "cJSON.h"
#include "controller_link.h"
#include "esp_app_desc.h"
#include "esp_event.h"
#include "esp_http_client.h"
#include <atomic>
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

QueueHandle_t actions;
namespace {
const char *TAG = "wifi_touchscreen";
Settings settings{};
State state{};
cl_session_t link{};
esp_websocket_client_handle_t ws;
QueueHandle_t frames;
struct Frame {
  int kind;
  size_t length;
  uint8_t bytes[CL_MAX_FRAME + 1];
};
char own_public[131], uri[180], pending_op[32];
uint8_t client_nonce[32];
uint32_t request_id, pending_id;
int64_t last_state, pending_since, last_ping, last_reading, last_age_update;
bool authenticated, traffic_ready;
std::atomic<bool> wifi_ready{false};
bool retry_connection = true;
int64_t next_connection_attempt;
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
    esp_err_t e = esp_wifi_connect();
    if (e != ESP_OK)
      ESP_LOGW(TAG, "Wi-Fi reconnect request failed: %s", esp_err_to_name(e));
  } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
    wifi_ready = true;
    ESP_LOGI(TAG,
             "Wi-Fi has an IP address; scale host='%s' paired=%d; connection retry enabled",
             settings.host[0] ? settings.host : "<none>", settings.paired);
    next_connection_attempt = now();
  }
}
void socket_event(void *, esp_event_base_t, int32_t event, void *data) {
  static Frame assembly{};
  Frame f{};
  if (event == WEBSOCKET_EVENT_CONNECTED) {
    ESP_LOGI(TAG, "WebSocket transport connected to %s", uri);
    f.kind = 1;
    if (xQueueSend(frames, &f, 0) != pdTRUE)
      ESP_LOGW(TAG, "Frame queue full while reporting WebSocket connection");
  } else if (event == WEBSOCKET_EVENT_DISCONNECTED) {
    ESP_LOGW(TAG, "WebSocket transport disconnected from %s", uri);
    f.kind = 2;
    if (xQueueSend(frames, &f, 0) != pdTRUE)
      ESP_LOGW(TAG, "Frame queue full while reporting WebSocket disconnect");
    assembly = {};
  } else if (event == WEBSOCKET_EVENT_ERROR) {
    ESP_LOGW(TAG, "WebSocket transport error while connecting to %s", uri);
  } else if (event == WEBSOCKET_EVENT_DATA) {
    auto *d = (esp_websocket_event_data_t *)data;
    if (d->op_code != 1 && d->op_code != 2)
      return;
    if (d->payload_offset == 0) {
      assembly = {};
      assembly.kind = d->op_code == 1 ? 3 : 4;
    }
    if (d->payload_len > CL_MAX_FRAME || d->payload_offset < 0 ||
        d->data_len < 0 || (size_t)d->payload_offset != assembly.length ||
        assembly.length + d->data_len > CL_MAX_FRAME) {
      ESP_LOGW(TAG,
               "Rejected WebSocket fragment: opcode=%d payload_len=%d offset=%d data_len=%d assembled=%u",
               d->op_code, d->payload_len, d->payload_offset, d->data_len,
               (unsigned)assembly.length);
      return;
    }
    memcpy(assembly.bytes + assembly.length, d->data_ptr, d->data_len);
    assembly.length += d->data_len;
    if (assembly.length == (size_t)d->payload_len) {
      ESP_LOGD(TAG, "Complete WebSocket message received: opcode=%d len=%u",
               d->op_code, (unsigned)assembly.length);
      if (xQueueSend(frames, &assembly, 0) != pdTRUE)
        ESP_LOGW(TAG, "Frame queue full; dropping WebSocket message");
    }
  }
}
void disconnected() {
  bool was_authenticated = authenticated;
  bool was_online = state.online;
  authenticated = false;
  traffic_ready = false;
  state.online = false;
  last_state = 0;
  cl_clear(&link);
  ui_state(state);
  if (was_authenticated || was_online)
    ESP_LOGI(TAG, "Scale session reset: authenticated=%d online=%d",
             was_authenticated, was_online);
  if (pending_id) {
    ESP_LOGW(TAG, "Connection lost with command id=%lu op='%s' pending",
             (unsigned long)pending_id, pending_op);
    ui_result(
        false, pending_op,
        "Connection lost. Outcome unknown; check the scale before retrying.");
    pending_id = 0;
  }
}
bool send_secure(const char *plain) {
  uint8_t out[CL_MAX_FRAME];
  size_t len = 0;
  esp_err_t e = cl_seal(&link, plain, out, &len);
  if (e != ESP_OK) {
    ESP_LOGW(TAG, "Could not encrypt WebSocket message: %s", esp_err_to_name(e));
    return false;
  }
  int sent = esp_websocket_client_send_bin(ws, (char *)out, len,
                                            pdMS_TO_TICKS(1000));
  if (sent != (int)len) {
    ESP_LOGW(TAG, "Encrypted WebSocket send incomplete: sent=%d expected=%u",
             sent, (unsigned)len);
    return false;
  }
  return true;
}
// Public setup status contains no credentials and cannot authorize a controller.
bool scale_accepting_connection() {
  char url[180], body[512] = {};
  snprintf(url, sizeof(url), "http://%s/api/controller", settings.host);
  ESP_LOGI(TAG, "Probing scale setup endpoint: %s", url);
  esp_http_client_config_t cfg = {};
  cfg.url = url;
  cfg.timeout_ms = 2000;
  auto client = esp_http_client_init(&cfg);
  if (!client) {
    ESP_LOGE(TAG, "Could not allocate HTTP client for scale probe");
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
           "Scale setup probe result: host='%s' open=%s headers=%lld status=%d bytes=%d",
           settings.host, esp_err_to_name(err), (long long)headers, status,
           received);
  if (received > 0)
    ESP_LOGI(TAG, "Scale setup response: %s", body);
  esp_http_client_close(client);
  esp_http_client_cleanup(client);
  if (status != 200 || received <= 0) {
    ui_message(status == 404
      ? "Update scale firmware to enable Wi-Fi touchscreen setup."
      : "Scale unreachable. Check its address and Wi-Fi network.");
    ESP_LOGW(TAG,
             "Scale setup probe failed: host='%s' status=%d open=%s headers=%lld bytes=%d",
             settings.host, status, esp_err_to_name(err), (long long)headers,
             received);
    return false;
  }
  auto o = cJSON_Parse(body);
  if (!o || !cJSON_IsBool(cJSON_GetObjectItemCaseSensitive(o, "paired"))) {
    ESP_LOGW(TAG, "Scale setup response was not expected controller JSON");
    cJSON_Delete(o);
    ui_message("Address did not return scale setup. Check the scale address.");
    return false;
  }
  bool paired = yes(o, "paired");
  bool pending = yes(o, "pending");
  double seconds = num(o, "seconds");
  bool connected = yes(o, "connected");
  bool ready = paired == settings.paired && (paired || seconds > 0);
  ESP_LOGI(TAG,
           "Scale controller status: scale_paired=%d display_paired=%d connected=%d pending=%d pairing_seconds=%.0f ready=%d",
           paired, settings.paired, connected, pending, seconds, ready);
  if (paired && !settings.paired)
    ui_message("On scale: remove old Wi-Fi touchscreen, then Add touchscreen.");
  else if (!paired && settings.paired)
    ui_message("In Setup: Remove pairing, then Add touchscreen on the scale.");
  else if (!ready)
    ui_message("On scale: Wi-Fi touchscreen setup > Add touchscreen.");
  cJSON_Delete(o);
  return ready;
}
void connect_scale() {
  retry_connection = true;
  next_connection_attempt = now() + 3000000;
  ESP_LOGI(TAG,
           "Scale connection attempt: host='%s' wifi_ready=%d display_paired=%d",
           settings.host[0] ? settings.host : "<none>", (bool)wifi_ready,
           settings.paired);
  if (ws) {
    ESP_LOGI(TAG, "Stopping previous WebSocket client before reconnect");
    esp_websocket_client_stop(ws);
    esp_websocket_client_destroy(ws);
    ws = nullptr;
  }
  disconnected();
  xQueueReset(frames);
  if (!settings.host[0]) {
    ESP_LOGW(TAG, "Scale connection skipped: no scale hostname/IP is saved");
    return;
  }
  if (!wifi_ready) {
    ESP_LOGW(TAG, "Scale connection deferred: Wi-Fi does not have an IP yet");
    ui_message("Waiting for Wi-Fi. Check SSID and password in Setup.");
    return;
  }
  if (!scale_accepting_connection()) {
    ESP_LOGW(TAG,
             "Scale connection stopped before WebSocket: setup endpoint is not accepting this display");
    return;
  }
  snprintf(uri, sizeof(uri), "ws://%s/ws/controller", settings.host);
  ESP_LOGI(TAG, "Starting WebSocket connection: uri='%s' paired=%d", uri,
           settings.paired);
  esp_websocket_client_config_t config = {};
  config.uri = uri;
  config.buffer_size = CL_MAX_FRAME + 1;
  config.task_stack = 6144;
  config.disable_auto_reconnect = true;
  config.reconnect_timeout_ms = 5000;
  config.network_timeout_ms = 5000;
  ws = esp_websocket_client_init(&config);
  if (!ws) {
    ESP_LOGE(TAG, "esp_websocket_client_init failed for %s", uri);
    ui_message("Could not start connection");
    return;
  }
  esp_websocket_register_events(ws, WEBSOCKET_EVENT_ANY, socket_event, nullptr);
  esp_err_t e = esp_websocket_client_start(ws);
  retry_connection = e != ESP_OK;
  ESP_LOGI(TAG, "WebSocket start returned %s; retry_connection=%d",
           esp_err_to_name(e), retry_connection);
  if (e != ESP_OK)
    ui_message("Could not start scale WebSocket connection");
}
void on_frame(const Frame &f) {
  if (f.kind == 2) {
    ESP_LOGW(TAG, "Processing WebSocket disconnect; retry in 3 seconds");
    retry_connection = true;
    next_connection_attempt = now() + 3000000;
    disconnected();
    ui_message("Disconnected — reconnecting to scale");
    return;
  }
  if (f.kind == 1) {
    ESP_LOGI(TAG, "WebSocket connected; beginning protocol-1 handshake");
    disconnected();
    char hello[320], nonce_hex[65];
    esp_fill_random(client_nonce, 32);
    cl_hex(client_nonce, 32, nonce_hex);
    if (settings.paired) {
      memcpy(link.master, settings.master, 32);
      snprintf(hello, sizeof(hello),
               "{\"type\":\"hello\",\"protocol\":1,\"nonce\":\"%s\"}",
               nonce_hex);
      ESP_LOGI(TAG, "Sending hello for saved pairing");
    } else {
      esp_err_t e = cl_keypair(&link, own_public);
      if (e != ESP_OK) {
        ESP_LOGE(TAG, "Pairing key initialization failed: %s", esp_err_to_name(e));
        ui_message("Pairing initialization failed");
        return;
      }
      snprintf(hello, sizeof(hello),
               "{\"type\":\"hello\",\"protocol\":1,\"public\":\"%s\",\"nonce\":"
               "\"%s\"}",
               own_public, nonce_hex);
      ESP_LOGI(TAG, "Sending hello for new pairing");
    }
    int sent = esp_websocket_client_send_text(ws, hello, strlen(hello),
                                               pdMS_TO_TICKS(1000));
    if (sent == (int)strlen(hello))
      ESP_LOGI(TAG, "Protocol hello sent successfully (%d bytes)", sent);
    else
      ESP_LOGW(TAG, "Protocol hello send failed/incomplete: sent=%d expected=%u",
               sent, (unsigned)strlen(hello));
    return;
  }
  char plain[CL_MAX_PLAIN];
  if (f.kind == 4) {
    esp_err_t e = cl_open(&link, f.bytes, f.length, plain);
    if (e != ESP_OK) {
      ESP_LOGW(TAG, "Scale encrypted frame authentication failed: %s",
               esp_err_to_name(e));
      ui_message("Scale authentication failed");
      return;
    }
  } else {
    if (f.length >= sizeof(plain)) {
      ESP_LOGW(TAG, "Plain WebSocket frame too large: %u", (unsigned)f.length);
      return;
    }
    memcpy(plain, f.bytes, f.length);
    plain[f.length] = 0;
  }
  cJSON *o = cJSON_Parse(plain);
  if (!o) {
    ESP_LOGW(TAG, "Scale WebSocket message is not valid JSON (kind=%d len=%u)",
             f.kind, (unsigned)f.length);
    return;
  }
  const char *type = str(o, "type");
  if (f.kind == 3 && !authenticated &&
      (!strcmp(type, "pair") || !strcmp(type, "challenge"))) {
    ESP_LOGI(TAG, "Received scale handshake message type='%s'", type);
    uint8_t challenge[32];
    if (num(o, "protocol") != 1 ||
        !cl_unhex(str(o, "challenge"), challenge, 32)) {
      ESP_LOGW(TAG, "Scale handshake validation failed: protocol=%.0f",
               num(o, "protocol"));
      cJSON_Delete(o);
      return;
    }
    bool pairing = !strcmp(type, "pair");
    esp_err_t e = ESP_OK;
    if (pairing) {
      char code[13];
      e = cl_agree(&link, str(o, "public"), str(o, "public"), own_public, code);
      if (e == ESP_OK) {
        ESP_LOGI(TAG,
                 "New-pairing key agreement succeeded; displaying 12-character approval code");
        ui_pair_code(code);
      }
    } else if (!settings.paired) {
      ESP_LOGW(TAG, "Scale sent saved-pairing challenge but display has no saved pairing");
      e = ESP_FAIL;
    }
    if (e == ESP_OK) {
      e = cl_start(&link, challenge, client_nonce, false);
      traffic_ready = e == ESP_OK;
      ESP_LOGI(TAG, "Traffic-key setup returned %s", esp_err_to_name(e));
    }
    if (e == ESP_OK && !pairing) {
      ESP_LOGI(TAG, "Sending encrypted auth proof for saved pairing");
      if (!send_secure("{\"type\":\"auth\"}"))
        ESP_LOGW(TAG, "Failed sending encrypted auth proof");
    }
    if (e != ESP_OK) {
      ESP_LOGW(TAG, "Pairing/authentication handshake failed: %s",
               esp_err_to_name(e));
      ui_message("Pairing failed. Reopen pairing on the scale.");
    }
  } else if (f.kind == 4 && !strcmp(type, "authorized")) {
    ESP_LOGI(TAG, "Scale authorized this touchscreen");
    if (!settings.paired) {
      memcpy(settings.master, link.master, 32);
      settings.paired = true;
      esp_err_t e = persist();
      if (e != ESP_OK) {
        settings.paired = false;
        ESP_LOGE(TAG, "Could not save new pairing: %s", esp_err_to_name(e));
        ui_message("Could not save pairing. Retry setup.");
        cJSON_Delete(o);
        return;
      }
      ESP_LOGI(TAG, "New scale pairing saved to NVS");
    }
    authenticated = true;
    retry_connection = false;
    ui_paired();
    ui_message("Connected to scale");
    ESP_LOGI(TAG, "Wi-Fi touchscreen session is authenticated and connected");
  } else if (f.kind == 4 && authenticated && !strcmp(type, "state")) {
    bool first_state = !state.online;
    last_state = now();
    last_reading = last_state;
    state.age_seconds = 0;
    state.online = true;
    state.valid = yes(o, "reading_valid");
    state.ready = yes(o, "ready");
    state.stable = yes(o, "stable");
    state.has_tare = yes(o, "has_tare");
    state.calibrated = yes(o, "calibrated");
    state.revision = num(o, "revision");
    state.weight = num(o, "weight");
    state.gallons = num(o, "gallons");
    state.servings = num(o, "servings");
    state.percent = num(o, "percent");
    state.capacity = num(o, "capacity");
    state.empty = num(o, "empty");
    state.density = num(o, "density");
    state.serving = num(o, "serving");
    snprintf(state.name, sizeof(state.name), "%s", str(o, "name"));
    snprintf(state.firmware, sizeof(state.firmware), "%s", str(o, "firmware"));
    if (first_state)
      ESP_LOGI(TAG,
               "First scale state received: firmware=%s ready=%d valid=%d stable=%d weight=%.3f lb revision=%lu",
               state.firmware, state.ready, state.valid, state.stable,
               (double)state.weight, (unsigned long)state.revision);
    ui_state(state);
  } else if (f.kind == 4 && authenticated && !strcmp(type, "result") &&
             num(o, "id") == pending_id && pending_id) {
    ESP_LOGI(TAG, "Scale command result received: id=%lu op='%s' ok=%d",
             (unsigned long)pending_id, pending_op, yes(o, "ok"));
    ui_result(yes(o, "ok"), pending_op, str(o, "error"));
    pending_id = 0;
  } else {
    ESP_LOGD(TAG,
             "Ignoring scale message type='%s' kind=%d authenticated=%d pending_id=%lu",
             type, f.kind, authenticated, (unsigned long)pending_id);
  }
  cJSON_Delete(o);
}
void action(const Action &a) {
  cJSON *o = cJSON_Parse(a.body);
  if (!strcmp(a.kind, "settings") && o) {
    const char *host = str(o, "host");
    bool valid = strlen(host) < sizeof(settings.host);
    for (const char *p = host; *p; p++)
      if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
            (*p >= '0' && *p <= '9') || *p == '.' || *p == '-'))
        valid = false;
    if (!valid || strlen(str(o, "ssid")) > 32 ||
        strlen(str(o, "password")) > 64) {
      ESP_LOGW(TAG, "Rejected settings update: invalid host/SSID/password length");
      ui_message("Enter a hostname or IPv4 address without http:// or a port");
    } else {
      bool host_changed = strcmp(host, settings.host) != 0;
      ESP_LOGI(TAG,
               "Saving connection settings: ssid='%s' host='%s' host_changed=%d previous_paired=%d brightness=%.0f",
               str(o, "ssid"), host, host_changed, settings.paired,
               num(o, "brightness"));
      if (host_changed) {
        settings.paired = false;
        memset(settings.master, 0, 32);
      }
      snprintf(settings.host, sizeof(settings.host), "%s", host);
      snprintf(settings.ssid, sizeof(settings.ssid), "%s", str(o, "ssid"));
      snprintf(settings.password, sizeof(settings.password), "%s",
               str(o, "password"));
      settings.brightness = (uint8_t)num(o, "brightness");
      if (persist() == ESP_OK) {
        ESP_LOGI(TAG, "Settings saved; restarting touchscreen");
        ui_message("Settings saved — restarting");
        vTaskDelay(pdMS_TO_TICKS(500));
        esp_restart();
      } else
        ui_message("Could not save settings");
    }
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
      for (unsigned i = 0; i < count; i++) {
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
    esp_err_t e = mdns_query_ptr("_kegscale", "_tcp", 3000, 4, &found);
    unsigned count = 0;
    mdns_result_t *only = nullptr;
    for (auto p = found; p; p = p->next)
      if (p->hostname) {
        count++;
        only = p;
        ESP_LOGI(TAG, "mDNS scale candidate: hostname='%s' port=%u", p->hostname,
                 p->port);
      }
    ESP_LOGI(TAG, "mDNS discovery completed: result=%s candidates=%u",
             esp_err_to_name(e), count);
    if (e == ESP_OK && count == 1) {
      char host[128];
      snprintf(host, sizeof(host), "%s.local", only->hostname);
      ESP_LOGI(TAG, "mDNS selected scale host='%s'", host);
      ui_discovered(host);
    } else
      ui_message(count > 1
                     ? "Several scales found. Enter the desired scale hostname."
                     : "No scale found. Enter its IP address or hostname.");
    if (found)
      mdns_query_results_free(found);
  } else if (!strcmp(a.kind, "forget")) {
    ESP_LOGI(TAG, "Removing saved touchscreen-side scale pairing");
    settings.paired = false;
    memset(settings.master, 0, 32);
    if (persist() == ESP_OK) {
      connect_scale();
      ui_message("Remove the old pairing on the scale, then Add touchscreen.");
    } else
      ui_message("Could not remove pairing");
  } else if (!strcmp(a.kind, "ota")) {
    if (pending_id)
      ui_message("Wait for the current operation to finish");
    else {
      ESP_LOGI(TAG, "Starting touchscreen OTA check");
      if (ws)
        esp_websocket_client_stop(ws);
      disconnected();
      esp_err_t e = touchscreen_ota();
      ESP_LOGI(TAG, "Touchscreen OTA returned %s", esp_err_to_name(e));
      if (e != ESP_OK)
        ui_message(esp_err_to_name(e));
      connect_scale();
    }
  } else if (!strcmp(a.kind, "command") && o) {
    if (!authenticated || !state.online) {
      ESP_LOGW(TAG, "Cannot send command '%s': authenticated=%d online=%d",
               str(o, "op"), authenticated, state.online);
      ui_result(false, str(o, "op"),
                "Scale disconnected. Reconnect before making changes.");
    } else if (pending_id) {
      ui_message("Wait for the previous operation to finish");
    } else {
      cJSON_AddStringToObject(o, "type", "command");
      cJSON_AddNumberToObject(o, "id", ++request_id);
      char *plain = cJSON_PrintUnformatted(o);
      snprintf(pending_op, sizeof(pending_op), "%s", str(o, "op"));
      pending_id = request_id;
      pending_since = now();
      ESP_LOGI(TAG, "Sending scale command: id=%lu op='%s'",
               (unsigned long)pending_id, pending_op);
      if (!plain || !send_secure(plain)) {
        ESP_LOGW(TAG, "Scale command send failed: id=%lu op='%s'",
                 (unsigned long)pending_id, pending_op);
        ui_result(false, pending_op,
                  "Send failed. Check the scale before retrying.");
        pending_id = 0;
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
    nvs_close(n);
  }
  settings.ssid[32] = 0;
  settings.password[64] = 0;
  settings.host[127] = 0;
  ESP_LOGI(TAG,
           "Loaded settings: ssid='%s' host='%s' paired=%d brightness=%u",
           settings.ssid[0] ? settings.ssid : "<none>",
           settings.host[0] ? settings.host : "<none>", settings.paired,
           settings.brightness);
  actions = xQueueCreate(4, sizeof(Action));
  frames = xQueueCreate(6, sizeof(Frame));
  configASSERT(actions && frames);
  ESP_ERROR_CHECK(psa_crypto_init() == PSA_SUCCESS ? ESP_OK : ESP_FAIL);
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
  connect_scale();
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
    if (retry_connection && settings.host[0] && now() >= next_connection_attempt)
      connect_scale();
    if (ws && esp_websocket_client_is_connected(ws) &&
        now() - last_ping > 3000000) {
      last_ping = now();
      if (traffic_ready && !send_secure("{\"type\":\"ping\"}"))
        ESP_LOGW(TAG, "Heartbeat send failed");
    }
    if (!state.online && last_reading && now() - last_age_update > 1000000) {
      last_age_update = now();
      state.age_seconds = (now() - last_reading) / 1000000;
      ui_state(state);
    }
    if (state.online && now() - last_state > 5000000) {
      ESP_LOGW(TAG, "Scale reading stale for >5 seconds; reconnecting");
      state.online = false;
      ui_state(state);
      ui_message("Reading is stale — reconnecting");
      connect_scale();
    }
    if (pending_id && now() - pending_since > 30000000) {
      ESP_LOGW(TAG, "Scale command timed out: id=%lu op='%s'",
               (unsigned long)pending_id, pending_op);
      ui_result(false, pending_op,
                "No confirmation. Check the scale before retrying.");
      pending_id = 0;
      connect_scale();
    }
  }
}
