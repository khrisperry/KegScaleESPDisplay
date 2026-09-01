#pragma once

#include <stddef.h>

#include "ble_client.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t display_ui_show_message(
    const char *title,
    const char *line1,
    const char *line2);

esp_err_t display_ui_show_candidates(
    const ble_client_peer_t *candidates,
    size_t count);

esp_err_t display_ui_show_scale(
    const ble_client_peer_t *peer,
    const ble_client_scale_state_t *state);

#ifdef __cplusplus
}
#endif
