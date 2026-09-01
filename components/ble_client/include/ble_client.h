#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define BLE_CLIENT_SCALE_ID_MAX 20
#define BLE_CLIENT_KEG_NAME_MAX 32
#define BLE_CLIENT_DEVICE_INFO_MAX 96
#define BLE_CLIENT_MAX_CANDIDATES 8
#define BLE_CLIENT_PROTOCOL_VERSION 1

typedef struct {
    char scale_id[BLE_CLIENT_SCALE_ID_MAX + 1];
    uint8_t address[6];
    uint8_t address_type;
    int8_t rssi;
    bool service_seen;
} ble_client_peer_t;

typedef struct {
    uint8_t protocol_version;
    uint8_t flags;
    uint16_t sequence;
    float remaining_percent;
    uint16_t remaining_servings;
    float remaining_gallons;
    float total_weight_lbs;
    float beverage_weight_lbs;
    int8_t wifi_rssi_dbm;
    uint8_t profile_revision;
    float serving_size_oz;
    uint8_t layout_id;
    uint8_t display_config_revision;
    uint8_t display_flags;
    char keg_name[BLE_CLIENT_KEG_NAME_MAX + 1];
    char device_info[BLE_CLIENT_DEVICE_INFO_MAX + 1];
} ble_client_scale_state_t;

enum {
    BLE_SCALE_FLAG_CALIBRATED = 1U << 0,
    BLE_SCALE_FLAG_STABLE = 1U << 1,
    BLE_SCALE_FLAG_PROFILE_CONFIGURED = 1U << 2,
    BLE_SCALE_FLAG_KEG_READY = 1U << 3,
    BLE_SCALE_FLAG_WIFI_CONNECTED = 1U << 4,
    BLE_SCALE_FLAG_TARE_SET = 1U << 5,
};

esp_err_t ble_client_init(void);

esp_err_t ble_client_scan(
    ble_client_peer_t *candidates,
    size_t capacity,
    size_t *count,
    uint32_t duration_ms);

esp_err_t ble_client_fetch(
    const ble_client_peer_t *peer,
    ble_client_scale_state_t *state);

bool ble_client_state_is_compatible(
    const ble_client_peer_t *peer,
    const ble_client_scale_state_t *state);

void ble_client_format_address(
    const ble_client_peer_t *peer,
    char *buffer,
    size_t buffer_size);

#ifdef __cplusplus
}
#endif
