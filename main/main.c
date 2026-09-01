#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "ble_client.h"
#include "display_ui.h"
#include "driver/gpio.h"
#include "esp_attr.h"
#include "esp_check.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "pairing.h"
#include "sdkconfig.h"

static const char *TAG = "display";

#define RETAINED_MAGIC 0x4B534450U
#define BUTTON_WAKE_GPIO GPIO_NUM_39
#define SIGNIFICANT_WEIGHT_LBS 0.5f

typedef struct {
    uint32_t magic;
    char scale_id[BLE_CLIENT_SCALE_ID_MAX + 1];
    uint16_t sequence;
    uint8_t profile_revision;
    uint16_t remaining_servings;
    float total_weight_lbs;
} retained_state_t;

RTC_DATA_ATTR static retained_state_t s_retained;

static void init_nvs(void)
{
    esp_err_t err = nvs_flash_init();

    if (err == ESP_ERR_NVS_NO_FREE_PAGES ||
        err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }

    ESP_ERROR_CHECK(err);
}

static const char *wake_reason(void)
{
    switch (esp_sleep_get_wakeup_cause()) {
        case ESP_SLEEP_WAKEUP_TIMER:
            return "timer";
        case ESP_SLEEP_WAKEUP_EXT0:
            return "button/touch";
        case ESP_SLEEP_WAKEUP_UNDEFINED:
            return "power/reset";
        default:
            return "other";
    }
}

static bool retained_matches_peer(
    const ble_client_peer_t *peer)
{
    return
        s_retained.magic == RETAINED_MAGIC &&
        strcmp(
            s_retained.scale_id,
            peer->scale_id) == 0;
}

static bool should_refresh(
    const ble_client_peer_t *peer,
    const ble_client_scale_state_t *state)
{
    if (!retained_matches_peer(peer)) {
        return true;
    }

    /*
     * Do not replace a stable e-paper image with an in-progress pour/movement.
     * The scale server increments sequence when the significant result settles.
     */
    if ((state->flags &
         BLE_SCALE_FLAG_STABLE) == 0) {
        return false;
    }

    if (state->sequence !=
            s_retained.sequence ||
        state->profile_revision !=
            s_retained.profile_revision ||
        state->remaining_servings !=
            s_retained.remaining_servings) {
        return true;
    }

    return fabsf(
               state->total_weight_lbs -
               s_retained.total_weight_lbs) >=
        SIGNIFICANT_WEIGHT_LBS;
}

static void remember_displayed_state(
    const ble_client_peer_t *peer,
    const ble_client_scale_state_t *state)
{
    memset(
        &s_retained,
        0,
        sizeof(s_retained));

    s_retained.magic =
        RETAINED_MAGIC;

    strlcpy(
        s_retained.scale_id,
        peer->scale_id,
        sizeof(s_retained.scale_id));

    s_retained.sequence =
        state->sequence;

    s_retained.profile_revision =
        state->profile_revision;

    s_retained.remaining_servings =
        state->remaining_servings;

    s_retained.total_weight_lbs =
        state->total_weight_lbs;
}

static void configure_wake_sources(void)
{
    ESP_ERROR_CHECK(
        esp_sleep_enable_timer_wakeup(
            (uint64_t)CONFIG_KEG_DISPLAY_SLEEP_SECONDS *
            1000000ULL));

#if CONFIG_KEG_DISPLAY_BUTTON_WAKE
    gpio_config_t button = {
        .pin_bit_mask =
            1ULL << BUTTON_WAKE_GPIO,
        .mode = GPIO_MODE_INPUT,
    };

    ESP_ERROR_CHECK(
        gpio_config(&button));

    /*
     * GPIO39 is input-only and the T5 board provides the button biasing.
     * Wait for release so a held button cannot create an immediate wake loop.
     */
    for (int i = 0;
         i < 100 &&
         gpio_get_level(BUTTON_WAKE_GPIO) == 0;
         ++i) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }

    ESP_ERROR_CHECK(
        esp_sleep_enable_ext0_wakeup(
            BUTTON_WAKE_GPIO,
            0));
#endif
}

static void go_to_sleep(void)
{
    configure_wake_sources();

    ESP_LOGI(
        TAG,
        "Sleeping for %d seconds; Button 1 also wakes the display",
        CONFIG_KEG_DISPLAY_SLEEP_SECONDS);

    fflush(stdout);
    vTaskDelay(pdMS_TO_TICKS(50));
    esp_deep_sleep_start();
}

static void print_candidate(
    size_t index,
    const ble_client_peer_t *peer)
{
    char address[24];

    ble_client_format_address(
        peer,
        address,
        sizeof(address));

    printf(
        "  %u. %-20s RSSI=%d address=%s type=%u\n",
        (unsigned)(index + 1),
        peer->scale_id,
        (int)peer->rssi,
        address,
        (unsigned)peer->address_type);
}

static esp_err_t scan_scales(
    ble_client_peer_t *candidates,
    size_t *count)
{
    printf(
        "Scanning %d seconds for Keg Scale BLE service...\n",
        CONFIG_KEG_DISPLAY_SCAN_SECONDS);

    esp_err_t err =
        ble_client_scan(
            candidates,
            BLE_CLIENT_MAX_CANDIDATES,
            count,
            CONFIG_KEG_DISPLAY_SCAN_SECONDS *
                1000U);

    if (err != ESP_OK) {
        printf(
            "Scan failed: %s\n",
            esp_err_to_name(err));
        return err;
    }

    if (*count == 0) {
        printf("No compatible Keg Scale found.\n");
    } else {
        printf(
            "Found %u compatible scale(s):\n",
            (unsigned)*count);

        for (size_t i = 0;
             i < *count;
             ++i) {
            print_candidate(
                i,
                &candidates[i]);
        }
    }

    return ESP_OK;
}

static esp_err_t validate_and_save_peer(
    const ble_client_peer_t *peer,
    ble_client_scale_state_t *state)
{
    esp_err_t err =
        ble_client_fetch(
            peer,
            state);

    if (err != ESP_OK) {
        return err;
    }

    if (!ble_client_state_is_compatible(
            peer,
            state)) {
        ESP_LOGW(
            TAG,
            "%s did not pass Keg Scale protocol identity validation",
            peer->scale_id);
        return ESP_ERR_INVALID_VERSION;
    }

    return pairing_save(peer);
}

static esp_err_t find_peer_by_id(
    const char *scale_id,
    ble_client_peer_t *peer)
{
    ble_client_peer_t candidates[
        BLE_CLIENT_MAX_CANDIDATES];

    size_t count = 0;

    esp_err_t err =
        scan_scales(
            candidates,
            &count);

    if (err != ESP_OK) {
        return err;
    }

    for (size_t i = 0;
         i < count;
         ++i) {
        if (strcmp(
                candidates[i].scale_id,
                scale_id) == 0) {
            *peer = candidates[i];
            return ESP_OK;
        }
    }

    return ESP_ERR_NOT_FOUND;
}

static esp_err_t fetch_paired_state(
    pairing_config_t *pairing,
    ble_client_scale_state_t *state)
{
    esp_err_t err =
        ble_client_fetch(
            &pairing->peer,
            state);

    if (err == ESP_OK &&
        ble_client_state_is_compatible(
            &pairing->peer,
            state)) {
        return ESP_OK;
    }

    /*
     * The logical KegScale-XXXX identity is stable. If the controller address
     * ever changes, scan for the exact saved ID and repair only that pairing.
     * Never fall back to strongest RSSI or a different scale.
     */
    ESP_LOGW(
        TAG,
        "Direct connection to %s failed; looking for the exact saved ID",
        pairing->peer.scale_id);

    ble_client_peer_t recovered = {0};

    err =
        find_peer_by_id(
            pairing->peer.scale_id,
            &recovered);

    if (err != ESP_OK) {
        return err;
    }

    err =
        ble_client_fetch(
            &recovered,
            state);

    if (err != ESP_OK ||
        !ble_client_state_is_compatible(
            &recovered,
            state)) {
        return err != ESP_OK ?
            err :
            ESP_ERR_INVALID_VERSION;
    }

    ESP_RETURN_ON_ERROR(
        pairing_save(&recovered),
        TAG,
        "Could not repair saved BLE address");

    pairing->peer = recovered;
    return ESP_OK;
}

static void render_if_needed(
    const ble_client_peer_t *peer,
    const ble_client_scale_state_t *state)
{
    if (!should_refresh(
            peer,
            state)) {
        ESP_LOGI(
            TAG,
            "No meaningful display change; keeping existing e-paper image");
        return;
    }

    esp_err_t err =
        display_ui_show_scale(
            peer,
            state);

    if (err == ESP_OK) {
        remember_displayed_state(
            peer,
            state);
    } else {
        ESP_LOGW(
            TAG,
            "Display refresh failed: %s",
            esp_err_to_name(err));
    }
}

static void print_help(void)
{
    printf(
        "\nSetup commands:\n"
        "  help                   Show commands\n"
        "  scan                   List compatible Keg Scales\n"
        "  pair KegScale-XXXX     Pair exact scale identity\n"
        "  unpair                 Clear saved scale\n"
        "  status                 Show saved pairing\n"
        "  sleep                  Sleep for normal timer interval\n\n");
}

static void trim_line(char *line)
{
    size_t length = strlen(line);

    while (length > 0 &&
           isspace(
               (unsigned char)line[length - 1])) {
        line[--length] = '\0';
    }

    char *start = line;

    while (*start != '\0' &&
           isspace((unsigned char)*start)) {
        ++start;
    }

    if (start != line) {
        memmove(
            line,
            start,
            strlen(start) + 1);
    }
}

static void setup_console(void)
{
    print_help();

    char line[96];

    while (true) {
        printf("display-setup> ");
        fflush(stdout);

        if (fgets(
                line,
                sizeof(line),
                stdin) == NULL) {
            clearerr(stdin);
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        trim_line(line);

        if (strcmp(line, "help") == 0) {
            print_help();
        } else if (
            strcmp(line, "scan") == 0) {
            ble_client_peer_t candidates[
                BLE_CLIENT_MAX_CANDIDATES];

            size_t count = 0;
            scan_scales(
                candidates,
                &count);

            if (count > 1) {
                display_ui_show_candidates(
                    candidates,
                    count);
            }
        } else if (
            strncmp(line, "pair ", 5) == 0) {
            const char *scale_id =
                line + 5;

            ble_client_peer_t peer = {0};

            esp_err_t err =
                find_peer_by_id(
                    scale_id,
                    &peer);

            if (err != ESP_OK) {
                printf(
                    "Could not find exact scale '%s'.\n",
                    scale_id);
                continue;
            }

            ble_client_scale_state_t state;

            err =
                validate_and_save_peer(
                    &peer,
                    &state);

            if (err != ESP_OK) {
                printf(
                    "Pairing failed: %s\n",
                    esp_err_to_name(err));
                continue;
            }

            printf(
                "Paired to %s. Rendering current keg state.\n",
                peer.scale_id);

            render_if_needed(
                &peer,
                &state);

            go_to_sleep();
        } else if (
            strcmp(line, "unpair") == 0) {
            esp_err_t err =
                pairing_clear();

            if (err == ESP_OK) {
                memset(
                    &s_retained,
                    0,
                    sizeof(s_retained));
                printf("Pairing cleared.\n");
            } else {
                printf(
                    "Unpair failed: %s\n",
                    esp_err_to_name(err));
            }
        } else if (
            strcmp(line, "status") == 0) {
            pairing_config_t pairing = {0};
            esp_err_t err =
                pairing_load(&pairing);

            if (err == ESP_OK &&
                pairing.paired) {
                char address[24];
                ble_client_format_address(
                    &pairing.peer,
                    address,
                    sizeof(address));

                printf(
                    "Paired: %s at %s\n",
                    pairing.peer.scale_id,
                    address);
            } else {
                printf("No scale paired.\n");
            }
        } else if (
            strcmp(line, "sleep") == 0) {
            go_to_sleep();
        } else if (
            line[0] != '\0') {
            printf(
                "Unknown command. Type 'help'.\n");
        }
    }
}

void app_main(void)
{
    ESP_LOGI(
        TAG,
        "KegScaleESPDisplay starting; wake=%s",
        wake_reason());

    init_nvs();

    ESP_ERROR_CHECK(
        ble_client_init());

    pairing_config_t pairing = {0};
    esp_err_t err =
        pairing_load(&pairing);

    if (err == ESP_OK &&
        pairing.paired) {
        ble_client_scale_state_t state;

        err =
            fetch_paired_state(
                &pairing,
                &state);

        if (err == ESP_OK) {
            render_if_needed(
                &pairing.peer,
                &state);
        } else {
            ESP_LOGW(
                TAG,
                "Paired scale %s unavailable: %s; keeping previous e-paper image",
                pairing.peer.scale_id,
                esp_err_to_name(err));

            if (!retained_matches_peer(
                    &pairing.peer)) {
                display_ui_show_message(
                    "KEG DISPLAY",
                    "SCALE OFFLINE",
                    pairing.peer.scale_id);
            }
        }

        go_to_sleep();
    }

    ble_client_peer_t candidates[
        BLE_CLIENT_MAX_CANDIDATES];

    size_t count = 0;

    err =
        scan_scales(
            candidates,
            &count);

#if CONFIG_KEG_DISPLAY_AUTO_PAIR_SINGLE
    if (err == ESP_OK &&
        count == 1) {
        ble_client_scale_state_t state;

        err =
            validate_and_save_peer(
                &candidates[0],
                &state);

        if (err == ESP_OK) {
            ESP_LOGI(
                TAG,
                "Exactly one compatible scale found; paired automatically to %s",
                candidates[0].scale_id);

            render_if_needed(
                &candidates[0],
                &state);

            go_to_sleep();
        }

        ESP_LOGW(
            TAG,
            "Single scale candidate did not validate: %s",
            esp_err_to_name(err));
    }
#endif

    if (count > 1) {
        display_ui_show_candidates(
            candidates,
            count);
    } else if (count == 0) {
        display_ui_show_message(
            "KEG DISPLAY",
            "NO SCALE FOUND",
            "CONNECT USB FOR SETUP");
    } else {
        display_ui_show_message(
            "KEG DISPLAY",
            "PAIRING NEEDED",
            candidates[0].scale_id);
    }

    setup_console();
}
