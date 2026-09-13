#include "app.h"
#include "cJSON.h"
#include "controller_link.h"
#include "esp_app_desc.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_ota_ops.h"
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
uint32_t request_id, pending_id;
int64_t last_state, pending_since, last_ping, last_reading, last_age_update;
bool authenticated, traffic_ready;
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
  if (e != ESP_OK)
    return e;
  e = nvs_set_blob(n, "settings", &settings, sizeof(settings));
  if (e == ESP_OK)
    e = nvs_commit(n);
  nvs_close(n);
  return e;
}
void wifi_event(void *, esp_event_base_t base, int32_t id, void *) {
  if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED)
    esp_wifi_connect();
}
void socket_event(void *, esp_event_base_t, int32_t event, void *data) {
  static Frame assembly{};
  Frame f{};
  if (event == WEBSOCKET_EVENT_CONNECTED) {
    f.kind = 1;
    xQueueSend(frames, &f, 0);
  } else if (event == WEBSOCKET_EVENT_DISCONNECTED) {
    f.kind = 2;
    xQueueSend(frames, &f, 0);
    assembly = {};
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
        assembly.length + d->data_len > CL_MAX_FRAME)
      return;
    memcpy(assembly.bytes + assembly.length, d->data_ptr, d->data_len);
    assembly.length += d->data_len;
    if (assembly.length == (size_t)d->payload_len)
      xQueueSend(frames, &assembly, 0);
  }
}
void disconnected() {
  authenticated = false;
  traffic_ready = false;
  state.online = false;
  last_state = 0;
  cl_clear(&link);
  ui_state(state);
  if (pending_id) {
    ui_result(
        false, pending_op,
        "Connection lost. Outcome unknown; check the scale before retrying.");
    pending_id = 0;
  }
}
bool send_secure(const char *plain) {
  uint8_t out[CL_MAX_FRAME];
  size_t len = 0;
  if (cl_seal(&link, plain, out, &len) != ESP_OK)
    return false;
  return esp_websocket_client_send_bin(ws, (char *)out, len,
                                       pdMS_TO_TICKS(1000)) == (int)len;
}
void connect_scale() {
  if (ws) {
    esp_websocket_client_stop(ws);
    esp_websocket_client_destroy(ws);
    ws = nullptr;
  }
  disconnected();
  xQueueReset(frames);
  if (!settings.host[0])
    return;
  snprintf(uri, sizeof(uri), "ws://%s/ws/controller", settings.host);
  esp_websocket_client_config_t config = {};
  config.uri = uri;
  config.buffer_size = CL_MAX_FRAME + 1;
  config.task_stack = 6144;
  config.reconnect_timeout_ms = 5000;
  config.network_timeout_ms = 5000;
  ws = esp_websocket_client_init(&config);
  if (!ws) {
    ui_message("Could not start connection");
    return;
  }
  esp_websocket_register_events(ws, WEBSOCKET_EVENT_ANY, socket_event, nullptr);
  esp_websocket_client_start(ws);
}
void on_frame(const Frame &f) {
  if (f.kind == 2) {
    disconnected();
    ui_message("Disconnected — reconnecting to scale");
    return;
  }
  if (f.kind == 1) {
    disconnected();
    char hello[220];
    if (settings.paired) {
      memcpy(link.master, settings.master, 32);
      snprintf(hello, sizeof(hello), "{\"type\":\"hello\",\"protocol\":1}");
    } else {
      if (cl_keypair(&link, own_public) != ESP_OK) {
        ui_message("Pairing initialization failed");
        return;
      }
      snprintf(hello, sizeof(hello),
               "{\"type\":\"hello\",\"protocol\":1,\"public\":\"%s\"}",
               own_public);
    }
    esp_websocket_client_send_text(ws, hello, strlen(hello),
                                   pdMS_TO_TICKS(1000));
    return;
  }
  char plain[CL_MAX_PLAIN];
  if (f.kind == 4) {
    if (cl_open(&link, f.bytes, f.length, plain) != ESP_OK) {
      ui_message("Scale authentication failed");
      return;
    }
  } else {
    if (f.length >= sizeof(plain))
      return;
    memcpy(plain, f.bytes, f.length);
    plain[f.length] = 0;
  }
  cJSON *o = cJSON_Parse(plain);
  if (!o)
    return;
  const char *type = str(o, "type");
  if (f.kind == 3 && !authenticated &&
      (!strcmp(type, "pair") || !strcmp(type, "challenge"))) {
    uint8_t challenge[32];
    if (num(o, "protocol") != 1 ||
        !cl_unhex(str(o, "challenge"), challenge, 32)) {
      cJSON_Delete(o);
      return;
    }
    bool pairing = !strcmp(type, "pair");
    esp_err_t e = ESP_OK;
    if (pairing) {
      char code[13];
      e = cl_agree(&link, str(o, "public"), str(o, "public"), own_public, code);
      if (e == ESP_OK)
        ui_pair_code(code);
    } else if (!settings.paired)
      e = ESP_FAIL;
    if (e == ESP_OK) {
      e = cl_start(&link, challenge, false);
      traffic_ready = e == ESP_OK;
    }
    if (e == ESP_OK && !pairing)
      send_secure("{\"type\":\"auth\"}");
    if (e != ESP_OK)
      ui_message("Pairing failed. Reopen pairing on the scale.");
  } else if (f.kind == 4 && !strcmp(type, "authorized")) {
    if (!settings.paired) {
      memcpy(settings.master, link.master, 32);
      settings.paired = true;
      if (persist() != ESP_OK) {
        settings.paired = false;
        ui_message("Could not save pairing. Retry setup.");
        cJSON_Delete(o);
        return;
      }
    }
    authenticated = true;
    ui_paired();
    ui_message("Connected to scale");
  } else if (f.kind == 4 && authenticated && !strcmp(type, "state")) {
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
    ui_state(state);
  } else if (f.kind == 4 && authenticated && !strcmp(type, "result") &&
             num(o, "id") == pending_id && pending_id) {
    ui_result(yes(o, "ok"), pending_op, str(o, "error"));
    pending_id = 0;
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
      ui_message("Enter a hostname or IPv4 address without http:// or a port");
    } else {
      if (strcmp(host, settings.host)) {
        settings.paired = false;
        memset(settings.master, 0, 32);
      }
      snprintf(settings.host, sizeof(settings.host), "%s", host);
      snprintf(settings.ssid, sizeof(settings.ssid), "%s", str(o, "ssid"));
      snprintf(settings.password, sizeof(settings.password), "%s",
               str(o, "password"));
      settings.brightness = (uint8_t)num(o, "brightness");
      if (persist() == ESP_OK) {
        ui_message("Settings saved — restarting");
        vTaskDelay(pdMS_TO_TICKS(500));
        esp_restart();
      } else
        ui_message("Could not save settings");
    }
  } else if (!strcmp(a.kind, "scan_wifi")) {
    wifi_scan_config_t scan = {};
    if (esp_wifi_scan_start(&scan, true) == ESP_OK) {
      wifi_ap_record_t aps[20];
      uint16_t count = 20;
      esp_wifi_scan_get_ap_records(&count, aps);
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
    } else
      ui_message("Wi-Fi scan failed. Enter the SSID manually.");
  } else if (!strcmp(a.kind, "discover")) {
    mdns_result_t *found = nullptr;
    esp_err_t e = mdns_query_ptr("_kegscale", "_tcp", 3000, 4, &found);
    unsigned count = 0;
    mdns_result_t *only = nullptr;
    for (auto p = found; p; p = p->next)
      if (p->hostname) {
        count++;
        only = p;
      }
    if (e == ESP_OK && count == 1) {
      char host[128];
      snprintf(host, sizeof(host), "%s.local", only->hostname);
      ui_discovered(host);
    } else
      ui_message(count > 1
                     ? "Several scales found. Enter the desired scale hostname."
                     : "No scale found. Enter its IP address or hostname.");
    if (found)
      mdns_query_results_free(found);
  } else if (!strcmp(a.kind, "forget")) {
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
      if (ws)
        esp_websocket_client_stop(ws);
      disconnected();
      esp_err_t e = touchscreen_ota();
      if (e != ESP_OK) {
        ui_message(esp_err_to_name(e));
        connect_scale();
      }
    }
  } else if (!strcmp(a.kind, "command") && o) {
    if (!authenticated || !state.online) {
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
      if (!plain || !send_secure(plain)) {
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
  esp_err_t e = nvs_flash_init();
  if (e == ESP_ERR_NVS_NO_FREE_PAGES || e == ESP_ERR_NVS_NEW_VERSION_FOUND) {
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
    }
    nvs_close(n);
  }
  settings.ssid[32] = 0;
  settings.password[64] = 0;
  settings.host[127] = 0;
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
  wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
  ESP_ERROR_CHECK(esp_wifi_init(&cfg));
  ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                             wifi_event, nullptr));
  ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
  ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
  wifi_config_t wifi = {};
  memcpy(wifi.sta.ssid, settings.ssid, strlen(settings.ssid));
  memcpy(wifi.sta.password, settings.password, strlen(settings.password));
  ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi));
  ESP_ERROR_CHECK(esp_wifi_start());
  esp_wifi_set_ps(WIFI_PS_NONE);
  if (settings.ssid[0])
    esp_wifi_connect();
  if (mdns_init() == ESP_OK)
    mdns_hostname_set(hostname);
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
    if (ws && esp_websocket_client_is_connected(ws) &&
        now() - last_ping > 3000000) {
      last_ping = now();
      if (traffic_ready)
        send_secure("{\"type\":\"ping\"}");
    }
    if (!state.online && last_reading && now() - last_age_update > 1000000) {
      last_age_update = now();
      state.age_seconds = (now() - last_reading) / 1000000;
      ui_state(state);
    }
    if (state.online && now() - last_state > 5000000) {
      state.online = false;
      ui_state(state);
      ui_message("Reading is stale — reconnecting");
      connect_scale();
    }
    if (pending_id && now() - pending_since > 30000000) {
      ui_result(false, pending_op,
                "No confirmation. Check the scale before retrying.");
      pending_id = 0;
      connect_scale();
    }
  }
}
