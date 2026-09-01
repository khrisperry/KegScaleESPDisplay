#include "display_ota.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "esp_crt_bundle.h"
#include "esp_event.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_ota_ops.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "psa/crypto.h"

static const char *TAG = "display_ota";

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAILED_BIT BIT1
#define WIFI_CONNECT_TIMEOUT_MS 20000
#define HTTP_TIMEOUT_MS 20000
#define DOWNLOAD_BUFFER_SIZE 4096
#define WIFI_MAX_RETRIES 5

typedef struct {
    EventGroupHandle_t events;
    unsigned retries;
} wifi_context_t;

static void secure_zero(void *value, size_t size)
{
    volatile uint8_t *bytes = value;

    while (size-- > 0) {
        *bytes++ = 0;
    }
}

static void wifi_event(
    void *arg,
    esp_event_base_t event_base,
    int32_t event_id,
    void *event_data)
{
    (void)event_data;
    wifi_context_t *context = arg;

    if (event_base == WIFI_EVENT &&
        event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT &&
               event_id ==
                   WIFI_EVENT_STA_DISCONNECTED) {
        if (context->retries < WIFI_MAX_RETRIES) {
            ++context->retries;
            esp_wifi_connect();
        } else {
            xEventGroupSetBits(
                context->events,
                WIFI_FAILED_BIT);
        }
    } else if (event_base == IP_EVENT &&
               event_id == IP_EVENT_STA_GOT_IP) {
        context->retries = 0;
        xEventGroupSetBits(
            context->events,
            WIFI_CONNECTED_BIT);
    }
}

static esp_err_t connect_wifi(
    const ble_client_update_bundle_t *bundle,
    esp_netif_t **netif,
    wifi_context_t *context,
    esp_event_handler_instance_t *wifi_handler,
    esp_event_handler_instance_t *ip_handler)
{
    esp_err_t err = esp_netif_init();

    if (err != ESP_OK &&
        err != ESP_ERR_INVALID_STATE) {
        return err;
    }

    err = esp_event_loop_create_default();

    if (err != ESP_OK &&
        err != ESP_ERR_INVALID_STATE) {
        return err;
    }

    *netif = esp_netif_create_default_wifi_sta();

    if (*netif == NULL) {
        return ESP_ERR_NO_MEM;
    }

    wifi_init_config_t init =
        WIFI_INIT_CONFIG_DEFAULT();

    err = esp_wifi_init(&init);

    if (err != ESP_OK) {
        return err;
    }

    context->events = xEventGroupCreate();

    if (context->events == NULL) {
        return ESP_ERR_NO_MEM;
    }

    err =
        esp_event_handler_instance_register(
            WIFI_EVENT,
            ESP_EVENT_ANY_ID,
            wifi_event,
            context,
            wifi_handler);

    if (err == ESP_OK) {
        err =
            esp_event_handler_instance_register(
                IP_EVENT,
                IP_EVENT_STA_GOT_IP,
                wifi_event,
                context,
                ip_handler);
    }

    wifi_config_t config = {0};
    const size_t ssid_len =
        strnlen(
            bundle->ssid,
            sizeof(config.sta.ssid));
    const size_t password_len =
        strnlen(
            bundle->password,
            sizeof(config.sta.password));

    memcpy(
        config.sta.ssid,
        bundle->ssid,
        ssid_len);
    memcpy(
        config.sta.password,
        bundle->password,
        password_len);
    config.sta.threshold.authmode =
        password_len > 0 ?
            WIFI_AUTH_WPA2_PSK :
            WIFI_AUTH_OPEN;
    config.sta.pmf_cfg.capable = true;
    config.sta.pmf_cfg.required = false;

    if (err == ESP_OK) {
        err =
            esp_wifi_set_storage(
                WIFI_STORAGE_RAM);
    }

    if (err == ESP_OK) {
        err =
            esp_wifi_set_mode(
                WIFI_MODE_STA);
    }

    if (err == ESP_OK) {
        err =
            esp_wifi_set_config(
                WIFI_IF_STA,
                &config);
    }

    secure_zero(&config, sizeof(config));

    if (err == ESP_OK) {
        err = esp_wifi_start();
    }

    if (err != ESP_OK) {
        return err;
    }

    EventBits_t bits =
        xEventGroupWaitBits(
            context->events,
            WIFI_CONNECTED_BIT |
                WIFI_FAILED_BIT,
            pdFALSE,
            pdFALSE,
            pdMS_TO_TICKS(
                WIFI_CONNECT_TIMEOUT_MS));

    return (bits & WIFI_CONNECTED_BIT) != 0 ?
        ESP_OK :
        ESP_ERR_TIMEOUT;
}

static void stop_wifi(
    esp_netif_t *netif,
    wifi_context_t *context,
    esp_event_handler_instance_t wifi_handler,
    esp_event_handler_instance_t ip_handler)
{
    esp_wifi_disconnect();
    esp_wifi_stop();
    esp_wifi_deinit();

    if (wifi_handler != NULL) {
        esp_event_handler_instance_unregister(
            WIFI_EVENT,
            ESP_EVENT_ANY_ID,
            wifi_handler);
    }

    if (ip_handler != NULL) {
        esp_event_handler_instance_unregister(
            IP_EVENT,
            IP_EVENT_STA_GOT_IP,
            ip_handler);
    }

    if (context->events != NULL) {
        vEventGroupDelete(context->events);
        context->events = NULL;
    }

    if (netif != NULL) {
        esp_netif_destroy_default_wifi(netif);
    }
}

static bool sha256_matches(
    const uint8_t digest[32],
    const char *expected)
{
    if (expected == NULL ||
        strlen(expected) != 64) {
        return false;
    }

    char actual[65];

    for (size_t i = 0; i < 32; ++i) {
        snprintf(
            actual + (i * 2),
            3,
            "%02x",
            digest[i]);
    }

    actual[64] = '\0';

    for (size_t i = 0; i < 64; ++i) {
        if ((char)tolower(
                (unsigned char)expected[i]) !=
            actual[i]) {
            return false;
        }
    }

    return true;
}

static esp_err_t download_and_stage(
    const ble_client_update_bundle_t *bundle)
{
    esp_http_client_config_t config = {
        .url = bundle->url,
        .timeout_ms = HTTP_TIMEOUT_MS,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .keep_alive_enable = false,
    };

    esp_http_client_handle_t client =
        esp_http_client_init(&config);

    if (client == NULL) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err =
        esp_http_client_open(client, 0);

    if (err != ESP_OK) {
        esp_http_client_cleanup(client);
        return err;
    }

    int64_t content_length =
        esp_http_client_fetch_headers(client);

    if (esp_http_client_get_status_code(client) !=
        200 ||
        (content_length > 0 &&
         bundle->size_bytes > 0 &&
         (uint64_t)content_length !=
             bundle->size_bytes)) {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return ESP_ERR_INVALID_RESPONSE;
    }

    const esp_partition_t *partition =
        esp_ota_get_next_update_partition(NULL);
    esp_ota_handle_t ota_handle = 0;

    if (partition == NULL) {
        err = ESP_ERR_NOT_FOUND;
    } else {
        err =
            esp_ota_begin(
                partition,
                bundle->size_bytes,
                &ota_handle);
    }

    psa_hash_operation_t sha =
        PSA_HASH_OPERATION_INIT;

    if (err == ESP_OK) {
        psa_status_t status = psa_crypto_init();

        if (status == PSA_SUCCESS) {
            status =
                psa_hash_setup(
                    &sha,
                    PSA_ALG_SHA_256);
        }

        if (status != PSA_SUCCESS) {
            err = ESP_FAIL;
        }
    }

    uint8_t buffer[DOWNLOAD_BUFFER_SIZE];
    uint32_t total = 0;

    while (err == ESP_OK) {
        int read =
            esp_http_client_read(
                client,
                (char *)buffer,
                sizeof(buffer));

        if (read < 0) {
            err = ESP_FAIL;
            break;
        }

        if (read == 0) {
            break;
        }

        if (UINT32_MAX - total <
            (uint32_t)read) {
            err = ESP_ERR_INVALID_SIZE;
            break;
        }

        total += (uint32_t)read;

        if (psa_hash_update(
                &sha,
                buffer,
                (size_t)read) != PSA_SUCCESS) {
            err = ESP_FAIL;
            break;
        }

        err =
            esp_ota_write(
                ota_handle,
                buffer,
                (size_t)read);

        vTaskDelay(1);
    }

    uint8_t digest[32] = {0};
    size_t digest_length = 0;

    if (err == ESP_OK &&
        psa_hash_finish(
            &sha,
            digest,
            sizeof(digest),
            &digest_length) != PSA_SUCCESS) {
        err = ESP_FAIL;
    }

    if (err != ESP_OK) {
        psa_hash_abort(&sha);
    }

    secure_zero(buffer, sizeof(buffer));

    if (err == ESP_OK &&
        (digest_length != sizeof(digest) ||
         total != bundle->size_bytes ||
         !sha256_matches(
             digest,
             bundle->sha256))) {
        err = ESP_ERR_INVALID_CRC;
    }

    secure_zero(digest, sizeof(digest));

    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    if (err == ESP_OK) {
        err = esp_ota_end(ota_handle);
    } else if (ota_handle != 0) {
        esp_ota_abort(ota_handle);
    }

    if (err == ESP_OK) {
        err =
            esp_ota_set_boot_partition(
                partition);
    }

    return err;
}

esp_err_t display_ota_confirm_running_image(void)
{
    const esp_partition_t *running =
        esp_ota_get_running_partition();

    if (running == NULL) {
        return ESP_ERR_NOT_FOUND;
    }

    esp_ota_img_states_t state;
    esp_err_t err =
        esp_ota_get_state_partition(
            running,
            &state);

    if (err == ESP_ERR_NOT_FOUND ||
        err == ESP_ERR_NOT_SUPPORTED) {
        return ESP_OK;
    }

    if (err == ESP_OK &&
        state == ESP_OTA_IMG_PENDING_VERIFY) {
        err =
            esp_ota_mark_app_valid_cancel_rollback();
    }

    return err;
}

esp_err_t display_ota_install(
    ble_client_update_bundle_t *bundle)
{
    if (bundle == NULL ||
        bundle->ssid[0] == '\0' ||
        strcmp(
            bundle->hardware,
            DISPLAY_OTA_HARDWARE_ID) != 0 ||
        bundle->version[0] == '\0' ||
        bundle->size_bytes == 0 ||
        strncmp(bundle->url, "https://", 8) != 0 ||
        strlen(bundle->sha256) != 64) {
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGI(
        TAG,
        "Installing display firmware %s (%lu bytes)",
        bundle->version,
        (unsigned long)bundle->size_bytes);

    esp_netif_t *netif = NULL;
    wifi_context_t wifi = {0};
    esp_event_handler_instance_t wifi_handler = NULL;
    esp_event_handler_instance_t ip_handler = NULL;

    esp_err_t err =
        connect_wifi(
            bundle,
            &netif,
            &wifi,
            &wifi_handler,
            &ip_handler);

    if (err == ESP_OK) {
        err = download_and_stage(bundle);
    }

    stop_wifi(
        netif,
        &wifi,
        wifi_handler,
        ip_handler);

    secure_zero(
        bundle->password,
        sizeof(bundle->password));
    secure_zero(
        bundle->ssid,
        sizeof(bundle->ssid));

    return err;
}
