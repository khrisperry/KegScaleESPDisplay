#pragma once

#include <stdio.h>
#include <string.h>

#include "ble_client.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    DISPLAY_OTA_AUTHORIZATION_NOT_APPROVED = 0,
    DISPLAY_OTA_AUTHORIZATION_METADATA_MISMATCH,
    DISPLAY_OTA_AUTHORIZATION_NOT_NEWER,
    DISPLAY_OTA_AUTHORIZATION_APPROVED,
} display_ota_authorization_result_t;

static inline bool display_ota_version_is_newer(
    const char *candidate,
    const char *current)
{
    if (candidate == NULL || current == NULL) {
        return false;
    }

    unsigned candidate_major = 0;
    unsigned candidate_minor = 0;
    unsigned candidate_patch = 0;
    unsigned current_major = 0;
    unsigned current_minor = 0;
    unsigned current_patch = 0;
    char trailing = '\0';

    if (sscanf(
            candidate,
            "V%u.%u.%u%c",
            &candidate_major,
            &candidate_minor,
            &candidate_patch,
            &trailing) != 3 ||
        sscanf(
            current,
            "V%u.%u.%u%c",
            &current_major,
            &current_minor,
            &current_patch,
            &trailing) != 3) {
        return false;
    }

    if (candidate_major != current_major) {
        return candidate_major > current_major;
    }
    if (candidate_minor != current_minor) {
        return candidate_minor > current_minor;
    }
    return candidate_patch > current_patch;
}

/*
 * An advertised update is not authorization. Installation is permitted only
 * after the protected bundle is fetched successfully and its immutable
 * identity exactly matches the advertised offer.
 */
static inline display_ota_authorization_result_t
display_ota_authorization_evaluate(
    esp_err_t fetch_result,
    const ble_client_update_offer_t *offer,
    const ble_client_update_bundle_t *bundle,
    const char *current_version)
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

    if (!display_ota_version_is_newer(
            offer->version,
            current_version)) {
        return DISPLAY_OTA_AUTHORIZATION_NOT_NEWER;
    }

    return DISPLAY_OTA_AUTHORIZATION_APPROVED;
}

#ifdef __cplusplus
}
#endif
