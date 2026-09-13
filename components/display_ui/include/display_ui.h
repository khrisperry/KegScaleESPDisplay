#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ble_client.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t display_ui_show_message(
    const char *title,
    const char *line1,
    const char *line2);

/* A dedicated calibration card. Begin with full_refresh=true once, then use
 * aligned partial updates until the final result has been shown. */
esp_err_t display_ui_show_calibration(
    const char *heading, const char *action, const char *detail,
    const char *hint, unsigned step, bool full_refresh);

esp_err_t display_ui_show_touch_acknowledged(void);
esp_err_t display_ui_clear_touch_acknowledged(void);

esp_err_t display_ui_show_pairing_code(
    const char *scale_id,
    uint32_t passkey);

esp_err_t display_ui_show_setup_qr(
    const char *scale_id,
    const char *ip_address);

esp_err_t display_ui_show_candidates(
    const ble_client_peer_t *candidates,
    size_t count);

esp_err_t display_ui_show_scale(
    const ble_client_peer_t *peer,
    const ble_client_scale_state_t *state,
    uint8_t battery_percent,
    bool force_full_refresh);

#ifdef __cplusplus
}
#endif
