#pragma once

#include "ble_client.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define DISPLAY_OTA_HARDWARE_ID "lilygo_t5_v2_3_1"

esp_err_t display_ota_confirm_running_image(void);

esp_err_t display_ota_install(
    ble_client_update_bundle_t *bundle);

#ifdef __cplusplus
}
#endif
