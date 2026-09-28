#include "ota_signature.h"

#include <ctype.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mbedtls/base64.h"
#include "psa/crypto.h"

#define OTA_SIGNATURE_FORMAT 1U
#define OTA_SIGNATURE_ALGORITHM "ECDSA-P256-SHA256"
#define OTA_SIGNATURE_RAW_BYTES 64U
#define OTA_PUBLIC_KEY_BYTES 65U

typedef struct {
    const char *channel;
    const char *key_id;
    const uint8_t *public_key;
} ota_signature_key_t;

static const uint8_t s_dev_public_key[OTA_PUBLIC_KEY_BYTES] = {
    0x04, 0xd1, 0xce, 0xd8, 0x6c, 0xf1, 0x29, 0x5c,
    0x3c, 0xc5, 0x03, 0x72, 0x18, 0x2d, 0x48, 0xda,
    0x7a, 0x0f, 0x2d, 0xb6, 0xc3, 0x97, 0xdf, 0xd2,
    0x88, 0x8e, 0x8d, 0x19, 0x89, 0x14, 0x0d, 0x0a,
    0x7e, 0x9c, 0x7f, 0x0f, 0xdb, 0x33, 0xa2, 0x70,
    0x65, 0x51, 0xfe, 0x76, 0x42, 0x9c, 0x89, 0xc7,
    0x3f, 0x32, 0xe4, 0x1e, 0x09, 0x42, 0x2b, 0xa9,
    0xd2, 0x4f, 0xd0, 0x2c, 0x28, 0x2d, 0x32, 0x9b,
    0xf5,
};

static const uint8_t s_production_public_key[OTA_PUBLIC_KEY_BYTES] = {
    0x04, 0xe8, 0x08, 0x4f, 0x71, 0x62, 0xa5, 0xa4,
    0x52, 0x5f, 0x5b, 0xa6, 0xec, 0x06, 0x22, 0x8c,
    0x8a, 0x7f, 0x52, 0xed, 0xf4, 0x3f, 0x1d, 0x66,
    0xf7, 0x7a, 0x87, 0x39, 0xe1, 0x4c, 0x04, 0x4d,
    0xd0, 0x6e, 0x76, 0x64, 0xd5, 0x0c, 0x99, 0x33,
    0xb4, 0x2d, 0xff, 0xcf, 0xe0, 0x0d, 0x36, 0xdd,
    0xc2, 0xa7, 0x23, 0x31, 0x21, 0x13, 0xd4, 0x0c,
    0x63, 0x62, 0xa7, 0x3b, 0xae, 0x31, 0x3c, 0xed,
    0x57,
};

static const ota_signature_key_t s_keys[] = {
    {
        .channel = "dev",
        .key_id = "b22d59da8a84e17f",
        .public_key = s_dev_public_key,
    },
    {
        .channel = "production",
        .key_id = "16330b21be490f46",
        .public_key = s_production_public_key,
    },
};

static const ota_signature_key_t *select_key(
    const char *channel)
{
    if (channel == NULL) {
        return NULL;
    }

    for (size_t i = 0; i < sizeof(s_keys) / sizeof(s_keys[0]); ++i) {
        if (strcmp(channel, s_keys[i].channel) == 0) {
            return &s_keys[i];
        }
    }

    return NULL;
}

static bool extract_json_string(
    const char *json,
    const char *key,
    char *value,
    size_t value_size)
{
    if (json == NULL ||
        key == NULL ||
        value == NULL ||
        value_size < 2) {
        return false;
    }

    char pattern[48];
    const int pattern_len =
        snprintf(
            pattern,
            sizeof(pattern),
            "\"%s\"",
            key);

    if (pattern_len <= 0 ||
        (size_t)pattern_len >= sizeof(pattern)) {
        return false;
    }

    const char *cursor = strstr(json, pattern);
    if (cursor == NULL) {
        return false;
    }

    cursor += strlen(pattern);
    while (isspace((unsigned char)*cursor)) {
        ++cursor;
    }
    if (*cursor++ != ':') {
        return false;
    }
    while (isspace((unsigned char)*cursor)) {
        ++cursor;
    }
    if (*cursor++ != '\"') {
        return false;
    }

    size_t out = 0;
    while (*cursor != '\0' && *cursor != '\"') {
        const unsigned char ch = (unsigned char)*cursor;
        if (ch < 0x20 ||
            *cursor == '\\' ||
            out + 1 >= value_size) {
            return false;
        }
        value[out++] = *cursor++;
    }

    if (*cursor != '\"' || out == 0) {
        return false;
    }

    value[out] = '\0';
    return true;
}

static bool extract_json_u32(
    const char *json,
    const char *key,
    uint32_t *value)
{
    if (json == NULL || key == NULL || value == NULL) {
        return false;
    }

    char pattern[48];
    const int pattern_len =
        snprintf(
            pattern,
            sizeof(pattern),
            "\"%s\"",
            key);

    if (pattern_len <= 0 ||
        (size_t)pattern_len >= sizeof(pattern)) {
        return false;
    }

    const char *cursor = strstr(json, pattern);
    if (cursor == NULL) {
        return false;
    }

    cursor += strlen(pattern);
    while (isspace((unsigned char)*cursor)) {
        ++cursor;
    }
    if (*cursor++ != ':') {
        return false;
    }
    while (isspace((unsigned char)*cursor)) {
        ++cursor;
    }

    char *end = NULL;
    const unsigned long parsed =
        strtoul(
            cursor,
            &end,
            10);

    if (end == cursor ||
        parsed > UINT32_MAX) {
        return false;
    }

    while (isspace((unsigned char)*end)) {
        ++end;
    }

    if (*end != ',' && *end != '}') {
        return false;
    }

    *value = (uint32_t)parsed;
    return true;
}

esp_err_t touchscreen_ota_signature_verify(
    const uint8_t *manifest,
    size_t manifest_len,
    const char *signature_json,
    const char *channel)
{
    if (manifest == NULL ||
        manifest_len == 0 ||
        signature_json == NULL ||
        channel == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    const ota_signature_key_t *key_info = select_key(channel);
    if (key_info == NULL) {
        return ESP_ERR_NOT_SUPPORTED;
    }

    uint32_t format = 0;
    char algorithm[32] = {0};
    char signature_channel[16] = {0};
    char key_id[24] = {0};
    char signature_b64[128] = {0};

    if (!extract_json_u32(signature_json, "format", &format) ||
        !extract_json_string(signature_json, "algorithm", algorithm, sizeof(algorithm)) ||
        !extract_json_string(signature_json, "channel", signature_channel, sizeof(signature_channel)) ||
        !extract_json_string(signature_json, "key_id", key_id, sizeof(key_id)) ||
        !extract_json_string(signature_json, "signature", signature_b64, sizeof(signature_b64))) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    if (format != OTA_SIGNATURE_FORMAT ||
        strcmp(algorithm, OTA_SIGNATURE_ALGORITHM) != 0 ||
        strcmp(signature_channel, channel) != 0 ||
        strcmp(key_id, key_info->key_id) != 0) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    uint8_t signature[OTA_SIGNATURE_RAW_BYTES] = {0};
    size_t signature_len = 0;
    if (mbedtls_base64_decode(
            signature,
            sizeof(signature),
            &signature_len,
            (const unsigned char *)signature_b64,
            strlen(signature_b64)) != 0 ||
        signature_len != sizeof(signature)) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    if (psa_crypto_init() != PSA_SUCCESS) {
        return ESP_FAIL;
    }

    psa_key_attributes_t attributes = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_type(
        &attributes,
        PSA_KEY_TYPE_ECC_PUBLIC_KEY(PSA_ECC_FAMILY_SECP_R1));
    psa_set_key_bits(&attributes, 256);
    psa_set_key_usage_flags(&attributes, PSA_KEY_USAGE_VERIFY_MESSAGE);
    psa_set_key_algorithm(
        &attributes,
        PSA_ALG_ECDSA(PSA_ALG_SHA_256));

    psa_key_id_t key = 0;
    psa_status_t status =
        psa_import_key(
            &attributes,
            key_info->public_key,
            OTA_PUBLIC_KEY_BYTES,
            &key);
    psa_reset_key_attributes(&attributes);

    if (status != PSA_SUCCESS) {
        memset(signature, 0, sizeof(signature));
        return ESP_FAIL;
    }

    status =
        psa_verify_message(
            key,
            PSA_ALG_ECDSA(PSA_ALG_SHA_256),
            manifest,
            manifest_len,
            signature,
            sizeof(signature));

    psa_destroy_key(key);
    memset(signature, 0, sizeof(signature));

    return status == PSA_SUCCESS ? ESP_OK : ESP_ERR_INVALID_CRC;
}
