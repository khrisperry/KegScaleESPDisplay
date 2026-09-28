#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "ota_signature.h"

static const char k_manifest[] =
    "{\n"
    "  \"version\": \"V1.4.0\",\n"
    "  \"commit\": \"d281babe2861fd29082a76b13bba8038d0316f40\",\n"
    "  \"branch\": \"dev\",\n"
    "  \"channel\": \"dev\",\n"
    "  \"target\": \"esp32s3\",\n"
    "  \"protocol_version\": 1,\n"
    "  \"requires_display_protocol_min\": 1,\n"
    "  \"requires_display_protocol_max\": 1,\n"
    "  \"url\": \"https://raw.githubusercontent.com/khrisperry/KegScaleFirmware/main/firmware/dev/esp32s3/keg_scale_esp-d281babe2861.bin\",\n"
    "  \"size\": 1618640,\n"
    "  \"sha256\": \"ffc85b1e7e99e9e5d599b478e7e67fe055146cc9af0cf68af78cee63dcd1cace\",\n"
    "  \"published_at\": \"2026-09-25T20:54:25Z\"\n"
    "}\n";

static const char k_signature[] =
    "{\n"
    "  \"format\": 1,\n"
    "  \"algorithm\": \"ECDSA-P256-SHA256\",\n"
    "  \"channel\": \"dev\",\n"
    "  \"key_id\": \"b22d59da8a84e17f\",\n"
    "  \"signature\": \"wDJgRaCSk9VQ8FMut9XUv0/bPf2d13Ge7BqqpQO3nThYAlG/XnlCI58n9JXMt8r5FfByurOO812j8YATJDAUKQ==\"\n"
    "}\n";

int main(void)
{
    assert(
        touchscreen_ota_signature_verify(
            (const uint8_t *)k_manifest,
            strlen(k_manifest),
            k_signature,
            "dev") == ESP_OK);

    char tampered_manifest[sizeof(k_manifest)];
    memcpy(
        tampered_manifest,
        k_manifest,
        sizeof(k_manifest));
    char *version =
        strstr(
            tampered_manifest,
            "V1.4.0");
    assert(version != NULL);
    version[5] = '1';

    assert(
        touchscreen_ota_signature_verify(
            (const uint8_t *)tampered_manifest,
            strlen(tampered_manifest),
            k_signature,
            "dev") != ESP_OK);

    assert(
        touchscreen_ota_signature_verify(
            (const uint8_t *)k_manifest,
            strlen(k_manifest),
            k_signature,
            "production") != ESP_OK);

    char altered_signature[sizeof(k_signature)];
    memcpy(
        altered_signature,
        k_signature,
        sizeof(k_signature));
    char *signature_value =
        strstr(
            altered_signature,
            "wDJgRa");
    assert(signature_value != NULL);
    signature_value[0] =
        signature_value[0] == 'A' ?
            'B' :
            'A';

    assert(
        touchscreen_ota_signature_verify(
            (const uint8_t *)k_manifest,
            strlen(k_manifest),
            altered_signature,
            "dev") != ESP_OK);

    assert(
        touchscreen_ota_signature_verify(
            (const uint8_t *)k_manifest,
            strlen(k_manifest),
            "{}",
            "dev") != ESP_OK);

    puts(
        "PASS: embedded Touch OTA verifier accepts valid signature and rejects "
        "tampered manifest, wrong channel, altered signature, and unsigned metadata");
    return 0;
}
