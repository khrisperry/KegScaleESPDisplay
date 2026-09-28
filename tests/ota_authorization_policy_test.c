#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "ota_authorization_policy.h"

static void initialize_matching_metadata(
    ble_client_update_offer_t *offer,
    ble_client_update_bundle_t *bundle)
{
    memset(offer, 0, sizeof(*offer));
    memset(bundle, 0, sizeof(*bundle));

    offer->valid = true;
    offer->size_bytes = 123456U;
    strcpy(offer->hardware, "waveshare_esp32_epaper");
    strcpy(offer->version, "V1.3.2");
    strcpy(
        offer->sha256,
        "0123456789abcdef0123456789abcdef"
        "0123456789abcdef0123456789abcdef");

    bundle->size_bytes = offer->size_bytes;
    strcpy(bundle->hardware, offer->hardware);
    strcpy(bundle->version, offer->version);
    strcpy(bundle->sha256, offer->sha256);
}

int main(void)
{
    ble_client_update_offer_t offer;
    ble_client_update_bundle_t bundle;

    initialize_matching_metadata(&offer, &bundle);

    assert(
        display_ota_authorization_evaluate(
            ESP_OK,
            &offer,
            &bundle,
            "V1.3.1") ==
        DISPLAY_OTA_AUTHORIZATION_APPROVED);

    assert(
        display_ota_authorization_evaluate(
            ESP_ERR_TIMEOUT,
            &offer,
            &bundle,
            "V1.3.1") ==
        DISPLAY_OTA_AUTHORIZATION_NOT_APPROVED);

    assert(
        display_ota_authorization_evaluate(
            ESP_OK,
            NULL,
            &bundle,
            "V1.3.1") ==
        DISPLAY_OTA_AUTHORIZATION_METADATA_MISMATCH);

    assert(
        display_ota_authorization_evaluate(
            ESP_OK,
            &offer,
            NULL,
            "V1.3.1") ==
        DISPLAY_OTA_AUTHORIZATION_METADATA_MISMATCH);

    offer.valid = false;
    assert(
        display_ota_authorization_evaluate(
            ESP_OK,
            &offer,
            &bundle,
            "V1.3.1") ==
        DISPLAY_OTA_AUTHORIZATION_METADATA_MISMATCH);
    offer.valid = true;

    strcpy(bundle.hardware, "wrong_hardware");
    assert(
        display_ota_authorization_evaluate(
            ESP_OK,
            &offer,
            &bundle,
            "V1.3.1") ==
        DISPLAY_OTA_AUTHORIZATION_METADATA_MISMATCH);
    strcpy(bundle.hardware, offer.hardware);

    strcpy(bundle.version, "V9.9.9");
    assert(
        display_ota_authorization_evaluate(
            ESP_OK,
            &offer,
            &bundle,
            "V1.3.1") ==
        DISPLAY_OTA_AUTHORIZATION_METADATA_MISMATCH);
    strcpy(bundle.version, offer.version);

    bundle.sha256[0] = bundle.sha256[0] == '0' ? '1' : '0';
    assert(
        display_ota_authorization_evaluate(
            ESP_OK,
            &offer,
            &bundle,
            "V1.3.1") ==
        DISPLAY_OTA_AUTHORIZATION_METADATA_MISMATCH);
    strcpy(bundle.sha256, offer.sha256);

    bundle.size_bytes++;
    assert(
        display_ota_authorization_evaluate(
            ESP_OK,
            &offer,
            &bundle,
            "V1.3.1") ==
        DISPLAY_OTA_AUTHORIZATION_METADATA_MISMATCH);
    bundle.size_bytes = offer.size_bytes;

    offer.version[0] = '\0';
    assert(
        display_ota_authorization_evaluate(
            ESP_OK,
            &offer,
            &bundle,
            "V1.3.1") ==
        DISPLAY_OTA_AUTHORIZATION_METADATA_MISMATCH);

    initialize_matching_metadata(&offer, &bundle);
    assert(display_ota_version_is_newer("V1.3.2", "V1.3.1"));
    assert(!display_ota_version_is_newer("V1.3.2", "V1.3.2"));
    assert(!display_ota_version_is_newer("V1.3.1", "V1.3.2"));
    assert(!display_ota_version_is_newer("bad", "V1.3.2"));
    assert(display_ota_version_is_newer(
        "V1.3.2",
        "V1.3.1-4-gabcdef"));

    assert(
        display_ota_authorization_evaluate(
            ESP_OK,
            &offer,
            &bundle,
            "V1.3.2") ==
        DISPLAY_OTA_AUTHORIZATION_NOT_NEWER);

    assert(
        display_ota_authorization_evaluate(
            ESP_OK,
            &offer,
            &bundle,
            "V1.4.0") ==
        DISPLAY_OTA_AUTHORIZATION_NOT_NEWER);

    puts(
        "PASS: unapproved, missing, mismatched, and approved "
        "eInk OTA authorization cases");
    return 0;
}
