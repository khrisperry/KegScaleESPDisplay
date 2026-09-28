#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t touchscreen_ota_signature_verify(
    const uint8_t *manifest,
    size_t manifest_len,
    const char *signature_json,
    const char *channel);

#ifdef __cplusplus
}
#endif
