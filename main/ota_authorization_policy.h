#pragma once

#include <string.h>

#include "ble_client.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    DISPLAY_OTA_AUTHORIZATION_NOT_APPROVED = 0,
    DISPLAY_OTA_AUTHORIZATION_METADATA_MISMATCH,
    DISPLAY_OTA_AUTHORIZATION_APPROVED,
} display_ota_authorization_result_t;

/*
 * An advertised update is not authorization. Installation is permitted only
 * after the protected bundle is fetched successfully and its immutable
 * identity exactly matches the advertised offer.
 */
static inline display_ota_authorization_result_t
display_ota_authorization_evaluate(
    esp_err_t fetch_result,
    const ble_client_update_offer_t *offer,
    const ble_client_update_bundle_t *bundle)
{
    if (fetch_result != ESP_OK) {
        return DISPLAY_OTA_AUTHORIZATION_NOT_APPROVED;
    }

    if (offer == NULL ||
        bundle == NULL ||
        !offer->valid ||
        offer->size_bytes == 0 ||
        offer->hardware[0] == '\0' ||
        offer->version[0] == '\0' ||
        offer->sha256[0] == '\0') {
        return DISPLAY_OTA_AUTHORIZATION_METADATA_MISMATCH;
    }

    if (strcmp(offer->hardware, bundle->hardware) != 0 ||
        strcmp(offer->version, bundle->version) != 0 ||
        strcmp(offer->sha256, bundle->sha256) != 0 ||
        offer->size_bytes != bundle->size_bytes) {
        return DISPLAY_OTA_AUTHORIZATION_METADATA_MISMATCH;
    }

    return DISPLAY_OTA_AUTHORIZATION_APPROVED;
}

#ifdef __cplusplus
}
#endif
