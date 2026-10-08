#include "tap_app.h"
#include "foam_capture.h"
#include "pour_history.h"
#include "cJSON.h"
#include "esp_event.h"
#include "esp_ota_ops.h"
#include "esp_app_format.h"
#include "esp_http_server.h"
#include "esp_mac.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/event_groups.h"
#include "lwip/sockets.h"
#include "nvs.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unistd.h>

extern esp_err_t touchpour_save_wifi(const char *, const char *);

namespace tap {
static esp_netif_t *sta_netif;
static char device_hostname[32];
extern const uint8_t page_start[] asm("_binary_index_html_start");
extern const uint8_t page_gzip_start[] asm("_binary_index_html_gz_start");
extern const uint8_t page_gzip_end[] asm("_binary_index_html_gz_end");
static const char *WEB_TAG = "tap_web";
static esp_err_t json_reply(httpd_req_t *r, cJSON *j) {
  char *s = cJSON_PrintUnformatted(j);
  cJSON_Delete(j);
  if (!s)
    return ESP_ERR_NO_MEM;
  httpd_resp_set_type(r, "application/json");
  httpd_resp_set_hdr(r, "Cache-Control", "no-store");
  auto e = httpd_resp_sendstr(r, s);
  cJSON_free(s);
  return e;
}
static esp_err_t error(httpd_req_t *r, const char *msg,
                       const char *code = "400 Bad Request") {
  httpd_resp_set_status(r, code);
  cJSON *j = cJSON_CreateObject();
  cJSON_AddStringToObject(j, "error", msg);
  return json_reply(r, j);
}
static cJSON *body(httpd_req_t *r) {
  char header[8];
  if (httpd_req_get_hdr_value_str(r, "X-Tap-Control", header, sizeof(header)) !=
          ESP_OK ||
      strcmp(header, "1"))
    return nullptr;
  if (r->content_len <= 0 || r->content_len > 4096)
    return nullptr;
  char b[4097];
  int got = 0;
  while (got < r->content_len) {
    int n = httpd_req_recv(r, b + got, r->content_len - got);
    if (n <= 0)
      return nullptr;
    got += n;
  }
  b[got] = 0;
  return cJSON_Parse(b);
}
static esp_err_t page(httpd_req_t *r) {
  // Browsers request icons separately; do not send the full UI for those.
  if (!strcmp(r->uri, "/favicon.ico")) {
    httpd_resp_set_status(r, "204 No Content");
    return httpd_resp_send(r, nullptr, 0);
  }
  if (!strncmp(r->uri, "/api/", 5))
    return error(r, "Endpoint not found", "404 Not Found");
  httpd_resp_set_type(r, "text/html");
  httpd_resp_set_hdr(r, "Cache-Control", "no-store");
  httpd_resp_set_hdr(r, "Vary", "Accept-Encoding");
  char encoding[128] = {};
  bool gzip = false;
  if (httpd_req_get_hdr_value_str(r, "Accept-Encoding", encoding,
                                 sizeof(encoding)) == ESP_OK) {
    char *save = nullptr;
    for (char *token = strtok_r(encoding, ",", &save); token;
         token = strtok_r(nullptr, ",", &save)) {
      while (*token == ' ' || *token == '\t') ++token;
      if (strncmp(token, "gzip", 4) ||
          (token[4] && token[4] != ';' && token[4] != ' ' && token[4] != '\t'))
        continue;
      const char *quality = strstr(token + 4, "q=");
      gzip = !quality || std::strtof(quality + 2, nullptr) > 0;
      break;
    }
  }
  const uint8_t *data = gzip ? page_gzip_start : page_start;
  size_t length = gzip ? (size_t)(page_gzip_end - page_gzip_start)
                       : strlen((const char *)page_start);
  if (gzip)
    httpd_resp_set_hdr(r, "Content-Encoding", "gzip");
  int64_t started = esp_timer_get_time();
  size_t sent = 0;
  esp_err_t result = ESP_OK;
  while (sent < length) {
    size_t remaining = length - sent;
    size_t chunk = remaining < 1024 ? remaining : 1024;
    result = httpd_resp_send_chunk(r, (const char *)data + sent, chunk);
    if (result != ESP_OK)
      break;
    sent += chunk;
  }
  if (result == ESP_OK)
    result = httpd_resp_send_chunk(r, nullptr, 0);
  wifi_ap_record_t ap{};
  bool connected = esp_wifi_sta_get_ap_info(&ap) == ESP_OK;
  ESP_LOGI(WEB_TAG, "Page %s: %u/%u bytes, %lld ms, result=%s, RSSI=%d dBm, "
                    "STA_connected=%d, free_heap=%u",
           gzip ? "gzip" : "plain", (unsigned)sent, (unsigned)length,
           (long long)((esp_timer_get_time() - started) / 1000),
           esp_err_to_name(result), connected ? ap.rssi : 0, connected,
           (unsigned)esp_get_free_heap_size());
  return result;
}
static void add_config(cJSON *j, const Settings &c) {
#define FIELD(n) cJSON_AddNumberToObject(j, #n, c.n)
  FIELD(closed_us);
  FIELD(open_us);
  FIELD(stop_mm);
  FIELD(min_mm);
  FIELD(max_mm);
  FIELD(tray_mm);
  FIELD(stable_mm);
  FIELD(jump_mm);
  FIELD(stale_ms);
  FIELD(max_pour_ms);
#undef FIELD
  cJSON_AddBoolToObject(j, "servo_calibrated", c.servo_calibrated);
  cJSON_AddBoolToObject(j, "distance_calibrated", c.distance_calibrated);
}
static esp_err_t status(httpd_req_t *r) {
  auto *j = cJSON_CreateObject();
  xSemaphoreTake(state_mutex, portMAX_DELAY);
  uint32_t now = esp_timer_get_time() / 1000;
  cJSON_AddStringToObject(j, "version", esp_app_get_description()->version);
  cJSON_AddStringToObject(j, "hostname", device_hostname);
  cJSON_AddStringToObject(j, "build", esp_app_get_description()->idf_ver);
  cJSON_AddBoolToObject(j, "updating", firmware_updating);
  const auto *next = esp_ota_get_next_update_partition(nullptr);
  cJSON_AddNumberToObject(j, "ota_max_bytes", next ? next->size : 0);
  cJSON_AddStringToObject(j, "state",
                          firmware_updating ? "UPDATING" : manual_hold.pouring ? "MANUAL HOLD" : pouring_active() ? "POURING"
                          : control.ready(settings, now) && !setup_mode &&
                                  !manual_test_active && !firmware_updating && !history_saving
                              ? "READY"
                              : "IDLE");
  cJSON_AddStringToObject(j, "reason", control.reason);
  cJSON_AddBoolToObject(j, "pouring", pouring_active());
  cJSON_AddBoolToObject(j, "manual_hold", manual_hold.pouring);
  cJSON_AddBoolToObject(j, "ready",
                        control.ready(settings, now) && !setup_mode &&
                            !manual_test_active && !firmware_updating && !history_saving && !pouring_active());
  cJSON_AddNumberToObject(j, "distance_mm", sample.mm);
  cJSON_AddNumberToObject(j, "sensor_status", sample.status);
  cJSON_AddNumberToObject(j, "age_ms", now - sample.at);
  cJSON_AddBoolToObject(j, "measurement_valid",
                        sample.valid &&
                            now - sample.at <= (uint32_t)settings.stale_ms);
  cJSON_AddNumberToObject(j, "stable_samples", manual_hold.pouring ? manual_hold.count : control.count);
  cJSON_AddNumberToObject(j, "servo_us", servo_us);
  cJSON_AddBoolToObject(j, "setup_mode", setup_mode.load());
  const auto recording = foam_status(now);
  auto *f = cJSON_AddObjectToObject(j, "foam_test");
  cJSON_AddBoolToObject(f, "recording", recording.active);
  cJSON_AddNumberToObject(f, "samples", recording.count);
  cJSON_AddNumberToObject(f, "elapsed_ms", recording.elapsed_ms);
  cJSON_AddNumberToObject(f, "max_samples", FoamCapture::capacity);
  cJSON_AddNumberToObject(f, "max_duration_ms", FoamCapture::duration_ms);
  auto *c = cJSON_AddObjectToObject(j, "settings");
  add_config(c, settings);
  xSemaphoreGive(state_mutex);
  cJSON_AddBoolToObject(j, "history_available", history_available());
  cJSON_AddBoolToObject(j, "history_saving", history_saving);
  auto *pours = cJSON_AddArrayToObject(j, "pours");
  PourSummary saved[5]; size_t count = history_list(saved);
  for (size_t i = 0; i < count; ++i) {
    auto *p = cJSON_CreateObject();
    cJSON_AddNumberToObject(p, "id", saved[i].id);
    cJSON_AddNumberToObject(p, "duration_ms", saved[i].duration_ms);
    cJSON_AddNumberToObject(p, "samples", saved[i].samples);
    cJSON_AddBoolToObject(p, "manual_hold", saved[i].manual_hold);
    cJSON_AddStringToObject(p, "reason", saved[i].reason);
    cJSON_AddItemToArray(pours, p);
  }
  esp_netif_ip_info_t ip = {};
  esp_netif_get_ip_info(sta_netif, &ip);
  char addr[20];
  snprintf(addr, sizeof(addr), IPSTR, IP2STR(&ip.ip));
  cJSON_AddStringToObject(j, "ip", addr);
  return json_reply(r, j);
}
static esp_err_t foam_action(httpd_req_t *r) {
  auto *j = body(r);
  if (!j) return error(r, "JSON and X-Tap-Control header required");
  auto *a = cJSON_GetObjectItemCaseSensitive(j, "action");
  if (!cJSON_IsString(a)) { cJSON_Delete(j); return error(r, "Action required"); }
  bool ok = false;
  uint32_t now = esp_timer_get_time() / 1000;
  xSemaphoreTake(state_mutex, portMAX_DELAY);
  if (!strcmp(a->valuestring, "start")) {
    auto f = foam_status(now);
    ok = !pouring_active() && !manual_test_active && !firmware_updating && !history_saving && !f.active;
    if (ok) foam_start(now);
  } else if (!strcmp(a->valuestring, "stop")) {
    foam_stop(now); ok = true;
  } else if (!strcmp(a->valuestring, "mark")) {
    auto *m = cJSON_GetObjectItemCaseSensitive(j, "marker");
    if (cJSON_IsString(m)) {
      uint8_t flag = !strcmp(m->valuestring, "foam_settling") ? FoamCapture::SETTLING
                     : !strcmp(m->valuestring, "cup_removed") ? FoamCapture::CUP_REMOVED
                     : !strcmp(m->valuestring, "cup_returned") ? FoamCapture::CUP_RETURNED : 0;
      if (flag) ok = foam_mark(flag);
    }
  }
  xSemaphoreGive(state_mutex);
  cJSON_Delete(j);
  if (!ok) return error(r, "Start with the tap idle; markers require an active recording", "409 Conflict");
  return status(r);
}
static esp_err_t foam_csv(httpd_req_t *r) {
  const auto recording = foam_status(esp_timer_get_time() / 1000);
  if (recording.active)
    return error(r, "Stop recording before downloading", "409 Conflict");
  if (!recording.count) return error(r, "No recorded readings available", "404 Not Found");
  httpd_resp_set_type(r, "text/csv; charset=utf-8");
  httpd_resp_set_hdr(r, "Cache-Control", "no-store");
  char filename[128];
  snprintf(filename, sizeof(filename), "attachment; filename=\"tap-foam-v%s.csv\"",
           esp_app_get_description()->version);
  httpd_resp_set_hdr(r, "Content-Disposition", filename);
  const char *header = "elapsed_ms,sequence,distance_mm,sensor_status,valid,interval_ms,delta_mm,rate_mm_per_s,controller_age_ms,pouring,ready,servo_test,button_pressed,servo_us,stable_samples,cutoff_mm,tray_mm,manual_hold,markers,reason,firmware\r\n";
  esp_err_t e = httpd_resp_sendstr_chunk(r, header);
  if (e != ESP_OK) return e;
  char chunk[2048], line[512];
  size_t used = 0;
  for (size_t i = 0; i < recording.count; ++i) {
    FoamRow row{};
    if (!foam_row(i, row)) return ESP_FAIL;
    char delta[32] = {}, rate[32] = {}, markers[64] = {};
    if (row.flags & FoamCapture::DELTA_VALID) {
      snprintf(delta, sizeof(delta), "%ld", (long)row.delta_mm);
      if (row.interval_ms)
        snprintf(rate, sizeof(rate), "%.3f", 1000.0 * row.delta_mm / row.interval_ms);
    }
    if (row.markers & FoamCapture::SETTLING) strcat(markers, "foam_settling|");
    if (row.markers & FoamCapture::CUP_REMOVED) strcat(markers, "cup_removed|");
    if (row.markers & FoamCapture::CUP_RETURNED) strcat(markers, "cup_returned|");
    if (markers[0]) markers[strlen(markers) - 1] = 0;
    int n = snprintf(line, sizeof(line), "%lu,%lu,%ld,%d,%d,%lu,%s,%s,%lu,%d,%d,%d,%d,%d,%d,%d,%d,%d,%s,\"%s\",%s\r\n",
        (unsigned long)row.elapsed_ms, (unsigned long)row.sequence, (long)row.mm, row.status,
        !!(row.flags & FoamCapture::VALID), (unsigned long)row.interval_ms, delta, rate,
        (unsigned long)row.age_ms, !!(row.flags & FoamCapture::POURING),
        !!(row.flags & FoamCapture::READY), !!(row.flags & FoamCapture::SERVO_TEST),
        !!(row.flags & FoamCapture::BUTTON), row.servo_us, row.stable_samples,
        row.cutoff_mm, row.tray_mm, !!(row.flags & FoamCapture::MANUAL_HOLD), markers, row.reason ? row.reason : "",
        esp_app_get_description()->version);
    if (n < 0 || (size_t)n >= sizeof(line)) return ESP_FAIL;
    if (used + n > sizeof(chunk)) {
      e = httpd_resp_send_chunk(r, chunk, used);
      if (e != ESP_OK) return e;
      used = 0;
    }
    memcpy(chunk + used, line, n); used += n;
  }
  if (used) { e = httpd_resp_send_chunk(r, chunk, used); if (e != ESP_OK) return e; }
  return httpd_resp_send_chunk(r, nullptr, 0);
}
static esp_err_t pour_csv(httpd_req_t *r) {
  char query[64], value[16];
  if (httpd_req_get_url_query_str(r, query, sizeof(query)) != ESP_OK ||
      httpd_query_key_value(query, "id", value, sizeof(value)) != ESP_OK)
    return error(r, "Pour id required");
  char *end; unsigned long id = strtoul(value, &end, 10);
  PourSummary summary{};
  if (!*value || *end || id == 0 || id > UINT32_MAX || !history_get(id, summary))
    return error(r, "Saved pour not found", "404 Not Found");
  httpd_resp_set_type(r, "text/csv");
  httpd_resp_set_hdr(r, "Cache-Control", "no-store");
  char attachment[80];
  snprintf(attachment, sizeof(attachment), "attachment; filename=\"tap-pour-%lu.csv\"", id);
  httpd_resp_set_hdr(r, "Content-Disposition", attachment);
  const char *header = "pour_id,elapsed_ms,sequence,distance_mm,sensor_status,valid,controller_age_ms,servo_us,manual_hold,button_event,cutoff_mm,cutoff_applied,min_mm,max_mm,tray_mm,stable_mm,jump_mm,stale_ms,max_pour_ms,closed_us,open_us,stop_reason,firmware\r\n";
  esp_err_t e = httpd_resp_sendstr_chunk(r, header);
  if (e != ESP_OK) return e;
  char chunk[2048], line[640]; size_t used = 0;
  const Settings &c = summary.settings;
  for (size_t i = 0; i < summary.samples; ++i) {
    PourPoint p{};
    if (!history_point(id, i, p)) return ESP_FAIL;
    int n = snprintf(line, sizeof(line), "%lu,%lu,%lu,%ld,%d,%u,%lu,%d,%u,%u,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,\"%s\",\"%s\"\r\n",
      id, (unsigned long)p.elapsed_ms, (unsigned long)p.sequence, (long)p.mm, p.status,
      p.valid, (unsigned long)p.age_ms, p.servo_us, p.manual_hold, p.button, c.stop_mm,
      !summary.manual_hold, c.min_mm, c.max_mm, c.tray_mm,
      c.stable_mm, c.jump_mm, c.stale_ms, c.max_pour_ms, c.closed_us, c.open_us,
      summary.reason, summary.firmware);
    if (n < 0 || (size_t)n >= sizeof(line)) return ESP_FAIL;
    if (used + (size_t)n > sizeof(chunk)) {
      e = httpd_resp_send_chunk(r, chunk, used); if (e != ESP_OK) return e; used = 0;
    }
    memcpy(chunk + used, line, n); used += n;
  }
  if (used) { e = httpd_resp_send_chunk(r, chunk, used); if (e != ESP_OK) return e; }
  return httpd_resp_send_chunk(r, nullptr, 0);
}
static esp_err_t configure(httpd_req_t *r) {
  auto *j = body(r);
  if (!j || !cJSON_IsObject(j)) {
    cJSON_Delete(j);
    return error(r, "JSON and X-Tap-Control header required");
  }
  xSemaphoreTake(state_mutex, portMAX_DELAY);
  Settings next = settings;
  bool ok = !pouring_active() && !manual_test_active && !firmware_updating && !history_saving;
#define FIELD(n)                                                               \
  {                                                                            \
    auto *v = cJSON_GetObjectItemCaseSensitive(j, #n);                         \
    if (v) {                                                                   \
      if (!cJSON_IsNumber(v) || !std::isfinite(v->valuedouble) ||              \
          v->valuedouble != v->valueint)                                       \
        ok = false;                                                            \
      else                                                                     \
        next.n = v->valueint;                                                  \
    }                                                                          \
  }
  FIELD(closed_us);
  FIELD(open_us);
  FIELD(stop_mm);
  FIELD(min_mm);
  FIELD(max_mm);
  FIELD(tray_mm);
  FIELD(stable_mm);
  FIELD(jump_mm);
  FIELD(stale_ms);
  FIELD(max_pour_ms);
#undef FIELD
  for (const char *k : {"servo_calibrated", "distance_calibrated"}) {
    auto *v = cJSON_GetObjectItemCaseSensitive(j, k);
    if (v) {
      if (!cJSON_IsBool(v))
        ok = false;
      else if (!strcmp(k, "servo_calibrated"))
        next.servo_calibrated = cJSON_IsTrue(v);
      else
        next.distance_calibrated = cJSON_IsTrue(v);
    }
  }
  ok &= valid_settings(next);
  if (ok)
    ok = save_settings(next);
  if (ok) {
    settings = next;
    control.stop("Settings saved: rechecking cup");
    xQueueReset(actions);
  }
  xSemaphoreGive(state_mutex);
  cJSON_Delete(j);
  if (!ok)
    return error(r, "Settings invalid, device pouring, or save failed");
  return status(r);
}
static esp_err_t action(httpd_req_t *r) {
  auto *j = body(r);
  if (!j)
    return error(r, "JSON and X-Tap-Control header required");
  auto *v = cJSON_GetObjectItemCaseSensitive(j, "action");
  if (!cJSON_IsString(v)) {
    cJSON_Delete(j);
    return error(r, "Action required");
  }
  bool ok = false;
  if (!strcmp(v->valuestring, "stop")) {
    stop_requested = true;
    ok = true;
  } else {
    xSemaphoreTake(state_mutex, portMAX_DELAY);
    Action a{};
    if (!strcmp(v->valuestring, "start")) {
      a.kind = Action::START;
      ok = !setup_mode && !pouring_active() && !manual_test_active &&
           control.ready(settings, esp_timer_get_time() / 1000);
    } else if (!strcmp(v->valuestring, "close")) {
      a.kind = Action::CLOSE;
      ok = !pouring_active();
    } else if (!strcmp(v->valuestring, "test")) {
      a.kind = Action::TEST;
      auto *p = cJSON_GetObjectItemCaseSensitive(j, "pulse_us");
      if (cJSON_IsNumber(p) && std::isfinite(p->valuedouble) &&
          p->valuedouble == p->valueint) {
        a.pulse = p->valueint;
        ok = !pouring_active() && a.pulse >= 500 && a.pulse <= 2500;
      }
    }
    if (firmware_updating || history_saving)
      ok = false;
    if (ok)
      ok = xQueueSend(actions, &a, 0) == pdTRUE;
    xSemaphoreGive(state_mutex);
  }
  cJSON_Delete(j);
  if (!ok)
    return error(r, "Action rejected: check readiness, range or active pour",
                 "409 Conflict");
  auto *out = cJSON_CreateObject();
  cJSON_AddBoolToObject(out, "accepted", true);
  return json_reply(r, out);
}
static esp_err_t scan(httpd_req_t *r) {
  auto *j = body(r);
  if (!j)
    return error(r, "JSON and X-Tap-Control header required");
  cJSON_Delete(j);
  xSemaphoreTake(state_mutex, portMAX_DELAY);
  bool busy = pouring_active() || manual_test_active || firmware_updating || history_saving;
  xSemaphoreGive(state_mutex);
  if (busy)
    return error(r, "Scan unavailable during a pour", "409 Conflict");
  wifi_scan_config_t c = {};
  c.show_hidden = false;
  if (esp_wifi_scan_start(&c, true) != ESP_OK)
    return error(r, "Wi-Fi scan failed");
  uint16_t n = 20;
  wifi_ap_record_t aps[20] = {};
  if (esp_wifi_scan_get_ap_records(&n, aps) != ESP_OK)
    return error(r, "Wi-Fi scan failed");
  auto *out = cJSON_CreateArray();
  for (int i = 0; i < n; i++) {
    auto *a = cJSON_CreateObject();
    cJSON_AddStringToObject(a, "ssid", (char *)aps[i].ssid);
    cJSON_AddNumberToObject(a, "rssi", aps[i].rssi);
    cJSON_AddBoolToObject(a, "secure", aps[i].authmode != WIFI_AUTH_OPEN);
    cJSON_AddItemToArray(out, a);
  }
  return json_reply(r, out);
}
static void reboot_task(void *) {
  vTaskDelay(pdMS_TO_TICKS(700));
  esp_restart();
}
// Raw application image only; never buffer the whole upload in C3 RAM.
static esp_err_t firmware_upload(httpd_req_t *r) {
  char header[8], type[48];
  if (httpd_req_get_hdr_value_str(r, "X-Tap-Control", header, sizeof(header)) != ESP_OK ||
      strcmp(header, "1") ||
      httpd_req_get_hdr_value_str(r, "Content-Type", type, sizeof(type)) != ESP_OK ||
      strcmp(type, "application/octet-stream"))
    return error(r, "Application binary and X-Tap-Control header required");
  const auto *partition = esp_ota_get_next_update_partition(nullptr);
  constexpr size_t prefix_size = sizeof(esp_image_header_t) +
      sizeof(esp_image_segment_header_t) + sizeof(esp_app_desc_t);
  if (!partition || r->content_len < prefix_size || r->content_len > partition->size)
    return error(r, "Firmware size invalid or OTA partition unavailable");
  xSemaphoreTake(state_mutex, portMAX_DELAY);
  bool busy = pouring_active() || manual_test_active || firmware_updating || history_saving ||
              foam_status(esp_timer_get_time() / 1000).active;
  if (!busy) {
    firmware_updating = true;
    update_closed = false;
    xQueueReset(actions);
  }
  xSemaphoreGive(state_mutex);
  if (busy)
    return error(r, "Stop pouring, servo tests and foam recording before updating", "409 Conflict");
  auto fail = [&](const char *message) {
    xSemaphoreTake(state_mutex, portMAX_DELAY);
    firmware_updating = false;
    update_closed = false;
    control.stop("Firmware update failed: new start required");
    xQueueReset(actions);
    xSemaphoreGive(state_mutex);
    return error(r, message);
  };
  bool closed = false;
  for (int i = 0; i < 100; ++i) {
    xSemaphoreTake(state_mutex, portMAX_DELAY);
    closed = update_closed;
    xSemaphoreGive(state_mutex);
    if (closed) break;
    vTaskDelay(pdMS_TO_TICKS(10));
  }
  if (!closed) return fail("Could not apply closed servo command");
  uint8_t buffer[2048];
  size_t received = 0;
  while (received < prefix_size) {
    int n = httpd_req_recv(r, (char *)buffer + received, prefix_size - received);
    if (n <= 0) return fail("Upload interrupted; current firmware retained");
    received += n;
  }
  esp_image_header_t image{};
  esp_app_desc_t desc{};
  memcpy(&image, buffer, sizeof(image));
  memcpy(&desc, buffer + sizeof(image) + sizeof(esp_image_segment_header_t), sizeof(desc));
  if (image.magic != ESP_IMAGE_HEADER_MAGIC || image.chip_id != CONFIG_IDF_FIRMWARE_CHIP_ID ||
      desc.magic_word != ESP_APP_DESC_MAGIC_WORD ||
      strncmp(desc.project_name, esp_app_get_description()->project_name, sizeof(desc.project_name)))
    return fail("Choose TapLidarMotor ESP32-C3 application .bin, not a full-flash image");
  esp_ota_handle_t handle;
  esp_err_t e = esp_ota_begin(partition, r->content_len, &handle);
  if (e != ESP_OK) return fail("Could not begin update; current firmware retained");
  e = esp_ota_write(handle, buffer, received);
  while (e == ESP_OK && received < r->content_len) {
    size_t remaining = r->content_len - received;
    int n = httpd_req_recv(r, (char *)buffer, remaining < sizeof(buffer) ? remaining : sizeof(buffer));
    if (n <= 0) { e = ESP_FAIL; break; }
    e = esp_ota_write(handle, buffer, n);
    received += n;
  }
  if (e != ESP_OK) {
    esp_ota_abort(handle);
    return fail("Upload interrupted or write failed; current firmware retained");
  }
  // esp_ota_end releases the handle even when validation fails.
  if (esp_ota_end(handle) != ESP_OK)
    return fail("Firmware validation failed; current firmware retained");
  if (esp_ota_set_boot_partition(partition) != ESP_OK)
    return fail("Could not select new firmware; current firmware retained");
  auto *out = cJSON_CreateObject();
  cJSON_AddStringToObject(out, "message", "Firmware installed. Restarting; Wi-Fi and calibration retained.");
  auto result = json_reply(r, out);
  // Keep maintenance latched until reboot, including if task allocation fails.
  if (xTaskCreate(reboot_task, "reboot", 2048, nullptr, 3, nullptr) != pdPASS)
    esp_restart();
  return result;
}
static esp_err_t wifi_save(httpd_req_t *r) {
  auto *j = body(r);
  if (!j)
    return error(r, "JSON and X-Tap-Control header required");
  bool reset = strstr(r->uri, "reset") != nullptr;
  auto *s = cJSON_GetObjectItemCaseSensitive(j, "ssid"),
       *p = cJSON_GetObjectItemCaseSensitive(j, "password");
  if (!reset && (!cJSON_IsString(s) || strlen(s->valuestring) < 1 ||
                 strlen(s->valuestring) > 32 || !cJSON_IsString(p) ||
                 strlen(p->valuestring) > 63 ||
                 (strlen(p->valuestring) > 0 && strlen(p->valuestring) < 8))) {
    cJSON_Delete(j);
    return error(r, "SSID/password invalid");
  }
  xSemaphoreTake(state_mutex, portMAX_DELAY);
  if (pouring_active() || manual_test_active || firmware_updating || history_saving) {
    xSemaphoreGive(state_mutex);
    cJSON_Delete(j);
    return error(r, "Stop pouring before changing Wi-Fi", "409 Conflict");
  }
  esp_err_t e = ::touchpour_save_wifi(reset ? "" : s->valuestring,
                                       reset ? "" : p->valuestring);
  if (e == ESP_OK) {
    setup_mode = true;
    stop_requested = true;
  }
  xSemaphoreGive(state_mutex);
  cJSON_Delete(j);
  if (e != ESP_OK)
    return error(r, "Could not save Wi-Fi");
  auto *out = cJSON_CreateObject();
  cJSON_AddStringToObject(out, "message",
                          "Saved. Rebooting; calibration retained.");
  auto result = json_reply(r, out);
  xTaskCreate(reboot_task, "reboot", 2048, nullptr, 3, nullptr);
  return result;
}
void network_start() {
  sta_netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
  uint8_t station_mac[6];
  ESP_ERROR_CHECK(esp_read_mac(station_mac, ESP_MAC_WIFI_STA));
  snprintf(device_hostname, sizeof(device_hostname), "TouchPour-%02X%02X", station_mac[4], station_mac[5]);
  httpd_config_t c = HTTPD_DEFAULT_CONFIG();
  c.max_uri_handlers = 12;
  c.stack_size = 8192;
  c.recv_wait_timeout = 10;
  c.send_wait_timeout = 15;
  c.keep_alive_enable = true;
  c.lru_purge_enable = true;
  c.uri_match_fn = httpd_uri_match_wildcard;
  httpd_handle_t server;
  esp_err_t started = httpd_start(&server, &c);
  if (started != ESP_OK) {
    ESP_LOGE(WEB_TAG, "HTTP server start failed: %s", esp_err_to_name(started));
    return;
  }
  struct Route {
    const char *uri;
    httpd_method_t method;
    esp_err_t (*handler)(httpd_req_t *);
  };
  Route routes[] = {{"/api/status", HTTP_GET, status},
                    {"/api/foam", HTTP_POST, foam_action},
                    {"/api/foam.csv", HTTP_GET, foam_csv},
                    {"/api/pours/download", HTTP_GET, pour_csv},
                    {"/api/firmware", HTTP_POST, firmware_upload},
                    {"/api/settings", HTTP_POST, configure},
                    {"/api/action", HTTP_POST, action},
                    {"/api/wifi/scan", HTTP_POST, scan},
                    {"/api/wifi/save", HTTP_POST, wifi_save},
                    {"/api/wifi/reset", HTTP_POST, wifi_save},
                    {"/*", HTTP_GET, page}};
  for (auto &r : routes) {
    httpd_uri_t u = {};
    u.uri = r.uri;
    u.method = r.method;
    u.handler = r.handler;
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &u));
  }
  ESP_LOGI(WEB_TAG, "HTTP server ready on port %u, firmware=%s, page=%u gzip bytes",
           c.server_port, esp_app_get_description()->version,
           (unsigned)(page_gzip_end - page_gzip_start));
}


} // namespace tap
