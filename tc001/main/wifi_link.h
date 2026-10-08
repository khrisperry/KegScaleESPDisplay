#pragma once
#include "esp_err.h"
#include "pixel_ui.h"
#include <stdint.h>
typedef struct {
    pixel_state_t state;
    char message[96];
    int64_t message_start_ms, last_state_ms;
    bool setup_active, online;
    char setup_ssid[33], setup_password[9];
    int64_t setup_start_ms;
} tc001_view_t;
esp_err_t tc001_wifi_start(void);
void tc001_wifi_setup_request(void);
void tc001_wifi_view(tc001_view_t *view);
