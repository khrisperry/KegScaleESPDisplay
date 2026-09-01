#include "pairing.h"

#include <string.h>

#include "esp_log.h"
#include "nvs.h"

static const char *TAG = "pairing";

#define PAIRING_NAMESPACE "scale_pair"
#define KEY_VALID "valid"
#define KEY_SCALE_ID "scale_id"
#define KEY_ADDR "addr"
#define KEY_ADDR_TYPE "addr_type"
#define KEY_LEGACY_PASSKEY "passkey"
#define KEY_POWER_CYCLES "pwr_cycles"
#define RECOVERY_POWER_CYCLES 3

esp_err_t pairing_load(pairing_config_t *config)
{
    if (config == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(config, 0, sizeof(*config));

    nvs_handle_t nvs;
    esp_err_t err =
        nvs_open(
            PAIRING_NAMESPACE,
            NVS_READONLY,
            &nvs);

    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    }

    if (err != ESP_OK) {
        return err;
    }

    uint8_t valid = 0;
    err = nvs_get_u8(
        nvs,
        KEY_VALID,
        &valid);

    if (err == ESP_ERR_NVS_NOT_FOUND ||
        valid != 1) {
        nvs_close(nvs);
        return ESP_OK;
    }

    size_t scale_id_size =
        sizeof(config->peer.scale_id);

    err =
        nvs_get_str(
            nvs,
            KEY_SCALE_ID,
            config->peer.scale_id,
            &scale_id_size);

    size_t addr_size =
        sizeof(config->peer.address);

    if (err == ESP_OK) {
        err =
            nvs_get_blob(
                nvs,
                KEY_ADDR,
                config->peer.address,
                &addr_size);
    }

    if (err == ESP_OK &&
        addr_size !=
            sizeof(config->peer.address)) {
        err = ESP_ERR_INVALID_SIZE;
    }

    if (err == ESP_OK) {
        err =
            nvs_get_u8(
                nvs,
                KEY_ADDR_TYPE,
                &config->peer.address_type);
    }

    nvs_close(nvs);

    if (err != ESP_OK) {
        ESP_LOGW(
            TAG,
            "Stored pairing is incomplete: %s",
            esp_err_to_name(err));
        memset(config, 0, sizeof(*config));
        return err;
    }

    config->paired = true;
    return ESP_OK;
}

esp_err_t pairing_save(
    const ble_client_peer_t *peer)
{
    if (peer == NULL ||
        peer->scale_id[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t nvs;
    esp_err_t err =
        nvs_open(
            PAIRING_NAMESPACE,
            NVS_READWRITE,
            &nvs);

    if (err != ESP_OK) {
        return err;
    }

    err =
        nvs_set_str(
            nvs,
            KEY_SCALE_ID,
            peer->scale_id);

    if (err == ESP_OK) {
        err =
            nvs_set_blob(
                nvs,
                KEY_ADDR,
                peer->address,
                sizeof(peer->address));
    }

    if (err == ESP_OK) {
        err =
            nvs_set_u8(
                nvs,
                KEY_ADDR_TYPE,
                peer->address_type);
    }

    if (err == ESP_OK) {
        err =
            nvs_set_u8(
                nvs,
                KEY_VALID,
                1);
    }

    if (err == ESP_OK) {
        esp_err_t legacy_err =
            nvs_erase_key(
                nvs,
                KEY_LEGACY_PASSKEY);

        if (legacy_err != ESP_OK &&
            legacy_err !=
                ESP_ERR_NVS_NOT_FOUND) {
            err = legacy_err;
        }
    }

    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }

    nvs_close(nvs);

    if (err == ESP_OK) {
        char address[24];
        ble_client_format_address(
            peer,
            address,
            sizeof(address));

        ESP_LOGI(
            TAG,
            "Paired scale %s at %s",
            peer->scale_id,
            address);
    }

    return err;
}

esp_err_t pairing_clear(void)
{
    nvs_handle_t nvs;
    esp_err_t err =
        nvs_open(
            PAIRING_NAMESPACE,
            NVS_READWRITE,
            &nvs);

    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    }

    if (err != ESP_OK) {
        return err;
    }

    err = nvs_erase_all(nvs);

    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }

    nvs_close(nvs);

    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Scale pairing cleared");
    }

    return err;
}

esp_err_t pairing_note_power_cycle(
    uint8_t *count,
    bool *recovery_requested)
{
    if (count == NULL ||
        recovery_requested == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    *count = 0;
    *recovery_requested = false;

    nvs_handle_t nvs;
    esp_err_t err =
        nvs_open(
            PAIRING_NAMESPACE,
            NVS_READWRITE,
            &nvs);

    if (err != ESP_OK) {
        return err;
    }

    uint8_t stored = 0;
    esp_err_t read_err =
        nvs_get_u8(
            nvs,
            KEY_POWER_CYCLES,
            &stored);

    if (read_err != ESP_OK &&
        read_err != ESP_ERR_NVS_NOT_FOUND) {
        nvs_close(nvs);
        return read_err;
    }

    if (stored < UINT8_MAX) {
        ++stored;
    }

    *count = stored;
    *recovery_requested =
        stored >= RECOVERY_POWER_CYCLES;

    if (*recovery_requested) {
        stored = 0;
    }

    err =
        nvs_set_u8(
            nvs,
            KEY_POWER_CYCLES,
            stored);

    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }

    nvs_close(nvs);
    return err;
}

esp_err_t pairing_reset_power_cycle_count(void)
{
    nvs_handle_t nvs;
    esp_err_t err =
        nvs_open(
            PAIRING_NAMESPACE,
            NVS_READWRITE,
            &nvs);

    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    }

    if (err != ESP_OK) {
        return err;
    }

    err =
        nvs_set_u8(
            nvs,
            KEY_POWER_CYCLES,
            0);

    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }

    nvs_close(nvs);
    return err;
}
