#include "app.h"
#include "cJSON.h"
#include "controller_link.h"
#include "esp_app_desc.h"
#include "esp_app_format.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_timer.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {
const char *TAG = "touchscreen_ota";
constexpr const char *feed_root =
    "https://raw.githubusercontent.com/khrisperry/KegScaleFirmware/main/touchscreen";

const char *text(cJSON *o, const char *key) {
  auto v = cJSON_GetObjectItemCaseSensitive(o, key);
  return cJSON_IsString(v) ? v->valuestring : "";
}

esp_http_client_handle_t open_url(const char *url) {
  char request_url[480];
  const char separator = strchr(url, '?') ? '&' : '?';
  const int request_len =
      snprintf(request_url, sizeof(request_url), "%s%ccache_bust=%llu", url,
               separator, (unsigned long long)esp_timer_get_time());
  if (request_len <= 0 || (size_t)request_len >= sizeof(request_url)) {
    ESP_LOGW(TAG, "OTA request URL too long after cache busting");
    return nullptr;
  }

  esp_http_client_config_t cfg = {};
  cfg.url = request_url;
  cfg.crt_bundle_attach = esp_crt_bundle_attach;
  cfg.timeout_ms = 15000;
  cfg.disable_auto_redirect = true;
  auto h = esp_http_client_init(&cfg);
  if (!h)
    return nullptr;

  esp_http_client_set_header(h, "Cache-Control",
                             "no-cache, no-store, max-age=0");
  esp_http_client_set_header(h, "Pragma", "no-cache");
  ESP_LOGI(TAG, "OTA cache-busted request: %s", request_url);

  esp_err_t e = esp_http_client_open(h, 0);
  int64_t headers = -1;
  int status = 0;
  if (e == ESP_OK) {
    headers = esp_http_client_fetch_headers(h);
    if (headers >= 0)
      status = esp_http_client_get_status_code(h);
  }
  if (e != ESP_OK || headers < 0 || status != 200) {
    ESP_LOGW(TAG, "OTA HTTP request failed: open=%s headers=%lld status=%d",
             esp_err_to_name(e), (long long)headers, status);
    esp_http_client_cleanup(h);
    return nullptr;
  }
  return h;
}

bool parse_version(const char *s, unsigned *major, unsigned *minor,
                   unsigned *patch) {
  if (!s || !major || !minor || !patch)
    return false;
  char extra = 0;
  return sscanf(s, "V%u.%u.%u%c", major, minor, patch, &extra) == 3;
}

int compare_versions(const char *a, const char *b) {
  unsigned amaj = 0, amin = 0, apat = 0;
  unsigned bmaj = 0, bmin = 0, bpat = 0;
  if (!parse_version(a, &amaj, &amin, &apat) ||
      !parse_version(b, &bmaj, &bmin, &bpat))
    return 0;
  if (amaj != bmaj)
    return amaj < bmaj ? -1 : 1;
  if (amin != bmin)
    return amin < bmin ? -1 : 1;
  if (apat != bpat)
    return apat < bpat ? -1 : 1;
  return 0;
}

void *ota_alloc(size_t size) {
  void *p = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!p)
    p = heap_caps_malloc(size, MALLOC_CAP_8BIT);
  return p;
}
} // namespace

esp_err_t touchscreen_ota(bool install) {
  ESP_LOGI(TAG,
           "OTA memory before %s: internal_free=%u largest_internal=%u psram_free=%u",
           install ? "install" : "check",
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
           (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));

  char channel[16] = {};
  touchscreen_ota_get_channel(channel, sizeof(channel));
  char base[300];
  snprintf(base, sizeof(base), "%s/%s/esp32s3/", feed_root, channel);

  char url[360];
  snprintf(url, sizeof(url), "%smanifest.json", base);
  ESP_LOGI(TAG, "Checking touchscreen %s feed: %s", channel, url);
  auto h = open_url(url);
  if (!h) {
    ESP_LOGW(TAG, "Could not open touchscreen OTA manifest for channel=%s",
             channel);
    return ESP_ERR_NOT_FOUND;
  }

  constexpr size_t kManifestSize = 4096;
  char *manifest = static_cast<char *>(ota_alloc(kManifestSize));
  if (!manifest) {
    esp_http_client_cleanup(h);
    ESP_LOGE(TAG, "Could not allocate OTA manifest buffer");
    return ESP_ERR_NO_MEM;
  }

  size_t used = 0;
  int n;
  while (used < kManifestSize - 1 &&
         (n = esp_http_client_read(h, manifest + used,
                                   kManifestSize - 1 - used)) > 0)
    used += n;
  bool complete = esp_http_client_is_complete_data_received(h);
  esp_http_client_cleanup(h);
  if (!complete) {
    free(manifest);
    ESP_LOGW(TAG, "Touchscreen OTA manifest download was incomplete");
    return ESP_ERR_INVALID_RESPONSE;
  }
  manifest[used] = 0;
  auto o = cJSON_Parse(manifest);
  free(manifest);
  if (!o)
    return ESP_ERR_INVALID_RESPONSE;

  const esp_partition_t *partition = esp_ota_get_next_update_partition(nullptr);
  auto size_item = cJSON_GetObjectItemCaseSensitive(o, "size");
  double size = cJSON_IsNumber(size_item) ? size_item->valuedouble : 0;
  uint8_t expected[32];
  char version[32];
  snprintf(version, sizeof(version), "%s", text(o, "version"));
  bool valid =
      partition && size > 0 && size <= partition->size && floor(size) == size &&
      !strcmp(text(o, "hardware"), "waveshare_esp32_s3_touch_lcd_4b") &&
      !strcmp(text(o, "target"), "esp32s3") &&
      !strcmp(text(o, "channel"), channel) &&
      !strncmp(text(o, "url"), base, strlen(base)) &&
      strlen(text(o, "url")) < sizeof(url) &&
      cl_unhex(text(o, "sha256"), expected, 32);
  auto protocol = cJSON_GetObjectItemCaseSensitive(o, "protocol_version");
  auto minimum =
      cJSON_GetObjectItemCaseSensitive(o, "requires_scale_wifi_protocol_min");
  auto maximum =
      cJSON_GetObjectItemCaseSensitive(o, "requires_scale_wifi_protocol_max");
  valid = valid && cJSON_IsNumber(protocol) && protocol->valuedouble == 1 &&
          cJSON_IsNumber(minimum) && minimum->valuedouble <= 1 &&
          cJSON_IsNumber(maximum) && maximum->valuedouble >= 1;
  snprintf(url, sizeof(url), "%s", text(o, "url"));
  cJSON_Delete(o);
  if (!valid) {
    ESP_LOGW(TAG,
             "Touchscreen OTA manifest failed identity/protocol/channel validation");
    return ESP_ERR_INVALID_RESPONSE;
  }

  const char *current = esp_app_get_description()->version;
  ESP_LOGI(TAG,
           "Touchscreen OTA feed: channel=%s current=%s latest=%s size=%u target_partition=%s mode=%s",
           channel, current, version, (unsigned)size, partition->label,
           install ? "install" : "check");

  unsigned current_major = 0, current_minor = 0, current_patch = 0;
  unsigned latest_major = 0, latest_minor = 0, latest_patch = 0;
  if (!parse_version(current, &current_major, &current_minor, &current_patch) ||
      !parse_version(version, &latest_major, &latest_minor, &latest_patch)) {
    ESP_LOGW(TAG,
             "Refusing OTA because firmware version format is invalid: current=%s latest=%s",
             current, version);
    return ESP_ERR_INVALID_VERSION;
  }

  const int version_cmp = compare_versions(version, current);
  if (version_cmp <= 0) {
    if (version_cmp < 0) {
      ESP_LOGW(TAG,
               "Ignoring stale OTA feed to prevent downgrade: current=%s feed=%s",
               current, version);
      ui_update_status(current, version, false, true);
    } else {
      ESP_LOGI(TAG, "Touchscreen firmware is already current");
      ui_update_status(current, version, false, false);
    }
    return ESP_OK;
  }

  ui_update_status(current, version, true, false);
  if (!install) {
    ESP_LOGI(TAG,
             "Touchscreen update is available; waiting for user install confirmation");
    return ESP_OK;
  }

  ui_update_installing(version);
  ui_update_progress(0);

  h = open_url(url);
  if (!h) {
    ESP_LOGW(TAG, "Could not open touchscreen firmware image: %s", url);
    return ESP_ERR_NOT_FOUND;
  }

  esp_ota_handle_t ota = 0;
  esp_err_t result = esp_ota_begin(partition, (size_t)size, &ota);
  psa_hash_operation_t hash = PSA_HASH_OPERATION_INIT;
  if (result == ESP_OK && psa_hash_setup(&hash, PSA_ALG_SHA_256) != PSA_SUCCESS)
    result = ESP_FAIL;

  constexpr size_t kBufferSize = 2048;
  uint8_t *buffer = static_cast<uint8_t *>(ota_alloc(kBufferSize));
  if (!buffer && result == ESP_OK)
    result = ESP_ERR_NO_MEM;

  size_t total = 0, header_used = 0;
  uint8_t header[sizeof(esp_image_header_t) +
                 sizeof(esp_image_segment_header_t) + sizeof(esp_app_desc_t)];
  bool identity_checked = false;
  while (result == ESP_OK && total < (size_t)size) {
    n = esp_http_client_read(h, (char *)buffer, kBufferSize);
    if (n <= 0) {
      result = ESP_ERR_INVALID_RESPONSE;
      break;
    }
    if (total + n > (size_t)size) {
      result = ESP_ERR_INVALID_SIZE;
      break;
    }
    if (!identity_checked) {
      size_t take = sizeof(header) - header_used;
      if (take > (size_t)n)
        take = n;
      memcpy(header + header_used, buffer, take);
      header_used += take;
      if (header_used == sizeof(header)) {
        esp_app_desc_t desc;
        memcpy(&desc,
               header + sizeof(esp_image_header_t) +
                   sizeof(esp_image_segment_header_t),
               sizeof(desc));
        identity_checked =
            desc.magic_word == ESP_APP_DESC_MAGIC_WORD &&
            !strncmp(desc.project_name, "keg_scale_touchscreen",
                     sizeof(desc.project_name)) &&
            !strncmp(desc.version, version, sizeof(desc.version));
        if (!identity_checked) {
          result = ESP_ERR_INVALID_VERSION;
          break;
        }
        ESP_LOGI(TAG, "OTA image identity verified: project=%s version=%s",
                 desc.project_name, desc.version);
      }
    }
    if (psa_hash_update(&hash, buffer, n) != PSA_SUCCESS) {
      result = ESP_FAIL;
      break;
    }
    result = esp_ota_write(ota, buffer, n);
    total += n;
    ui_update_progress((int)(total * 100 / (size_t)size));
  }

  if (buffer)
    free(buffer);

  uint8_t actual[32];
  size_t digest_len = 0;
  if (result == ESP_OK &&
      (total != (size_t)size || !identity_checked ||
       psa_hash_finish(&hash, actual, 32, &digest_len) != PSA_SUCCESS ||
       digest_len != 32 || memcmp(actual, expected, 32)))
    result = ESP_ERR_INVALID_CRC;
  psa_hash_abort(&hash);
  esp_http_client_cleanup(h);
  if (result == ESP_OK) {
    ESP_LOGI(TAG, "OTA download complete; SHA-256 verified (%u bytes)",
             (unsigned)total);
    result = esp_ota_end(ota);
    ota = 0;
  }
  if (result == ESP_OK)
    result = esp_ota_set_boot_partition(partition);
  if (ota)
    esp_ota_abort(ota);
  if (result == ESP_OK) {
    ESP_LOGI(TAG, "Touchscreen OTA staged successfully; rebooting into %s",
             version);
    ui_update_complete(version);
    vTaskDelay(pdMS_TO_TICKS(900));
    esp_restart();
  } else {
    ESP_LOGW(TAG, "Touchscreen OTA failed: %s", esp_err_to_name(result));
  }
  return result;
}
