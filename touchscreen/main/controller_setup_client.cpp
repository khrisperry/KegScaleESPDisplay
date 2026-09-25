#include "controller_setup_client.h"

#include "cJSON.h"
#include "esp_http_client.h"
#include <cstdio>
#include <cstring>

esp_err_t controller_setup_get(const char *host, int timeout_ms,
                               ControllerSetupResponse *response) {
  if (!host || !host[0] || !response)
    return ESP_ERR_INVALID_ARG;

  *response = {};
  response->headers = -1;
  response->bytes_received = -1;

  char url[192];
  snprintf(url, sizeof(url), "http://%s/api/controller", host);

  esp_http_client_config_t cfg = {};
  cfg.url = url;
  cfg.timeout_ms = timeout_ms;
  esp_http_client_handle_t client = esp_http_client_init(&cfg);
  if (!client)
    return ESP_ERR_NO_MEM;

  response->open_result = esp_http_client_open(client, 0);
  if (response->open_result == ESP_OK) {
    response->headers = esp_http_client_fetch_headers(client);
    if (response->headers >= 0) {
      response->http_status = esp_http_client_get_status_code(client);
      response->bytes_received = esp_http_client_read_response(
          client, response->body, sizeof(response->body) - 1);
      if (response->bytes_received > 0) {
        response->body[response->bytes_received] = 0;
        cJSON *json = cJSON_Parse(response->body);
        if (json) {
          response->json_valid = true;
          cJSON *paired =
              cJSON_GetObjectItemCaseSensitive(json, "paired");
          cJSON *connected =
              cJSON_GetObjectItemCaseSensitive(json, "connected");
          cJSON *pending =
              cJSON_GetObjectItemCaseSensitive(json, "pending");
          cJSON *seconds =
              cJSON_GetObjectItemCaseSensitive(json, "seconds");

          response->has_paired = cJSON_IsBool(paired);
          response->paired = cJSON_IsTrue(paired);
          response->connected = cJSON_IsTrue(connected);
          response->pending = cJSON_IsTrue(pending);
          response->has_seconds = cJSON_IsNumber(seconds);
          if (response->has_seconds)
            response->seconds = seconds->valuedouble;
          cJSON_Delete(json);
        }
      }
    }
  }

  esp_http_client_close(client);
  esp_http_client_cleanup(client);
  return response->open_result;
}

esp_err_t controller_setup_remove(const char *host, int timeout_ms,
                                  int *http_status) {
  if (http_status)
    *http_status = 0;
  if (!host || !host[0])
    return ESP_ERR_INVALID_ARG;

  char url[192];
  snprintf(url, sizeof(url), "http://%s/api/controller", host);
  static const char body[] = "{\"action\":\"remove\"}";

  esp_http_client_config_t cfg = {};
  cfg.url = url;
  cfg.timeout_ms = timeout_ms;
  esp_http_client_handle_t client = esp_http_client_init(&cfg);
  if (!client)
    return ESP_ERR_NO_MEM;

  esp_http_client_set_method(client, HTTP_METHOD_POST);
  esp_http_client_set_header(client, "Content-Type", "application/json");
  esp_http_client_set_header(client, "X-Controller-Setup", "1");
  esp_http_client_set_post_field(client, body, strlen(body));

  const esp_err_t e = esp_http_client_perform(client);
  if (http_status && e == ESP_OK)
    *http_status = esp_http_client_get_status_code(client);
  esp_http_client_cleanup(client);
  return e;
}
