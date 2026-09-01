#pragma once

#include <stdbool.h>

#include "ble_client.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool paired;
    uint32_t pairing_passkey;
    ble_client_peer_t peer;
} pairing_config_t;

esp_err_t pairing_load(pairing_config_t *config);
esp_err_t pairing_save(
    const ble_client_peer_t *peer,
    uint32_t pairing_passkey);
esp_err_t pairing_clear(void);

#ifdef __cplusplus
}
#endif
