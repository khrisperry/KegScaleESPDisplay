#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "ble_client.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool paired;
    ble_client_peer_t peer;
} pairing_config_t;

esp_err_t pairing_load(pairing_config_t *config);
esp_err_t pairing_save(
    const ble_client_peer_t *peer);
esp_err_t pairing_clear(void);

esp_err_t pairing_note_power_cycle(
    uint8_t *count,
    bool *recovery_requested);
esp_err_t pairing_reset_power_cycle_count(void);

#ifdef __cplusplus
}
#endif
