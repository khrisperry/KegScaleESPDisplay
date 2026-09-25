#pragma once

#include "esp_err.h"
#include <cstdint>

struct ControllerSetupResponse {
  esp_err_t open_result = ESP_FAIL;
  int64_t headers = -1;
  int http_status = 0;
  int bytes_received = -1;
  char body[512] = {};
  bool json_valid = false;
  bool has_paired = false;
  bool paired = false;
  bool connected = false;
  bool pending = false;
  bool has_seconds = false;
  double seconds = 0;
};

// Fetch and parse the Scale's local controller setup status. Transport/HTTP
// metadata is preserved so callers can keep their existing diagnostics and
// decide which JSON fields are mandatory for their own workflow.
esp_err_t controller_setup_get(const char *host, int timeout_ms,
                               ControllerSetupResponse *response);

// Request removal of the Scale-side Wi-Fi touchscreen pairing. Returns the
// transport result and, when available, the HTTP status through http_status.
esp_err_t controller_setup_remove(const char *host, int timeout_ms,
                                  int *http_status);
