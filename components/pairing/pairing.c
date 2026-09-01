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
#define KEY_PASSKEY "passkey"

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
    err = nvs_get_u8(nvs, KEY_VALID, &valid);

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

    if (err == ESP_OK) {
        esp_err_t passkey_err =
            nvs_get_u32(
                nvs,
                KEY_PASSKEY,
                &config->pairing_passkey);

        if (passkey_err == ESP_ERR_NVS_NOT_FOUND) {
            config->pairing_passkey = 0;
        } else if (passkey_err != ESP_OK) {
            err = passkey_err;
        }
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
    const ble_client_peer_t *peer,
    uint32_t pairing_passkey)
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
            nvs_set_u32(
                nvs,
                KEY_PASSKEY,
                pairing_passkey);
    }

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
