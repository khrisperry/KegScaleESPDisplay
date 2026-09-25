#include "setup_discovery.h"

#include "app.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mdns.h"
#include <atomic>
#include <cstdio>
#include <cstring>
#include <new>

namespace {
const char *TAG = "wifi_touchscreen";
constexpr uint32_t kSetupScanTaskStackBytes = 6144;
constexpr UBaseType_t kSetupScanTaskPriority = 4;
std::atomic<bool> wifi_scan_running{false};
std::atomic<bool> scale_scan_running{false};

struct ScanRequest {
  uint32_t generation;
};

void finish_request(ScanRequest *request, std::atomic<bool> &running) {
  delete request;
  running = false;
  vTaskDelete(nullptr);
}

void wifi_scan_task(void *arg) {
  auto *request = static_cast<ScanRequest *>(arg);
  const uint32_t generation = request ? request->generation : 0;

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
      const char *ssid = reinterpret_cast<const char *>(aps[i].ssid);
      if (!ssid[0] || strchr(ssid, '\n'))
        continue;
      if (options[0])
        strcat(options, "\n");
      strncat(options, ssid, 32);
    }
    ui_networks(options[0] ? options : "No networks found", generation);
    ui_message_for_generation("Choose your Wi-Fi network", generation);
  } else {
    ESP_LOGW(TAG, "Wi-Fi scan failed: %s", esp_err_to_name(scan_err));
    ui_message_for_generation(
        "Wi-Fi scan failed. Enter the SSID manually.", generation);
  }

  finish_request(request, wifi_scan_running);
}

void scale_scan_task(void *arg) {
  auto *request = static_cast<ScanRequest *>(arg);
  const uint32_t generation = request ? request->generation : 0;

  ESP_LOGI(TAG, "Starting mDNS discovery for _kegscale._tcp");
  mdns_result_t *found = nullptr;
  esp_err_t e = mdns_query_ptr("_kegscale", "_tcp", 3000, 8, &found);
  char options[800];
  const unsigned count =
      e == ESP_OK
          ? setup_discovery_build_scale_options(found, options,
                                                sizeof(options))
          : 0;
  if (e != ESP_OK)
    snprintf(options, sizeof(options), "Manual IP / hostname...");
  ESP_LOGI(TAG, "mDNS discovery completed: result=%s candidates=%u",
           esp_err_to_name(e), count);

  ui_discovered_options_for_generation(options, generation);
  if (count > 1)
    ui_message_for_generation(
        "Several scales found. Choose one from the list.", generation);
  else if (count == 1)
    ui_message_for_generation(
        "Scale found. Choose it or use Manual IP entry.", generation);
  else
    ui_message_for_generation(
        "No scale found. Choose Manual IP / hostname.", generation);

  if (found)
    mdns_query_results_free(found);
  finish_request(request, scale_scan_running);
}

bool start_scan(uint32_t generation, std::atomic<bool> &running,
                TaskFunction_t task, const char *task_name) {
  if (running.exchange(true))
    return false;

  auto *request = new (std::nothrow) ScanRequest{generation};
  if (!request) {
    running = false;
    ESP_LOGW(TAG, "Could not allocate %s request", task_name);
    return false;
  }

  BaseType_t created =
      xTaskCreate(task, task_name, kSetupScanTaskStackBytes, request,
                  kSetupScanTaskPriority, nullptr);
  if (created != pdPASS) {
    delete request;
    running = false;
    ESP_LOGW(TAG, "Could not create %s worker", task_name);
    return false;
  }
  return true;
}
} // namespace

unsigned setup_discovery_build_scale_options(mdns_result_t *found,
                                             char *options,
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

bool setup_discovery_start_wifi_scan(uint32_t ui_generation) {
  return start_scan(ui_generation, wifi_scan_running, wifi_scan_task,
                    "wifi_scan");
}

bool setup_discovery_start_scale_scan(uint32_t ui_generation) {
  return start_scan(ui_generation, scale_scan_running, scale_scan_task,
                    "scale_scan");
}
