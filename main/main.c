#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ble_client.h"
#include "display_ota.h"
#include "display_ui.h"
#include "driver/gpio.h"
#include "esp_attr.h"
#include "esp_app_desc.h"
#include "esp_check.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_sleep.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "pairing.h"
#include "sdkconfig.h"
#include "touch_wake.h"

static const char *TAG = "display";

#define RETAINED_MAGIC 0x4B534450U
#define SIGNIFICANT_WEIGHT_LBS 0.5f

typedef struct {
    uint32_t magic;
    char scale_id[BLE_CLIENT_SCALE_ID_MAX + 1];
    uint16_t sequence;
    uint8_t profile_revision;
    uint8_t display_config_revision;
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

static bool is_touch_wake(void)
{
    return
        (esp_sleep_get_wakeup_causes() &
         BIT(ESP_SLEEP_WAKEUP_TOUCHPAD)) != 0;
}

static const char *wake_reason(void)
{
    const uint32_t causes =
        esp_sleep_get_wakeup_causes();

    if (causes &
        BIT(ESP_SLEEP_WAKEUP_TOUCHPAD)) {
        return "touch";
    }

    if (causes &
        BIT(ESP_SLEEP_WAKEUP_TIMER)) {
        return "timer";
    }

    if (causes == 0) {
        return "power/reset";
    }

    return "other";
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
        state->display_config_revision !=
            s_retained.display_config_revision ||
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

    s_retained.display_config_revision =
        state->display_config_revision;

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

#if CONFIG_KEG_DISPLAY_TOUCH_WAKE
    ESP_ERROR_CHECK(
        touch_wake_prepare());
#endif
}

static void go_to_sleep(void)
{
    pairing_reset_power_cycle_count();
    configure_wake_sources();

#if CONFIG_KEG_DISPLAY_TOUCH_WAKE
    ESP_LOGI(
        TAG,
        "Sleeping for %d seconds; GPIO12 capacitive touch also wakes the display",
        CONFIG_KEG_DISPLAY_SLEEP_SECONDS);
#else
    ESP_LOGI(
        TAG,
        "Sleeping for %d seconds",
        CONFIG_KEG_DISPLAY_SLEEP_SECONDS);
#endif

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
        /*
         * Pairing security has just completed and ble_client_pair() requests
         * a disconnect before returning. That disconnect is asynchronous, so
         * an immediate validation reconnect can briefly collide with the
         * controller teardown. Give it one deliberate retry before treating
         * the newly-created bond as failed.
         */
        ESP_LOGW(
            TAG,
            "Initial post-pair validation connection failed; retrying once");

        vTaskDelay(pdMS_TO_TICKS(750));

        err =
            ble_client_fetch(
                peer,
                state);
    }

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
     * A visible scale can occasionally miss a connection establishment even
     * though its saved address is still correct. Retry the known address once
     * before spending several seconds scanning for identity recovery.
     */
    ESP_LOGW(
        TAG,
        "Direct connection to %s failed; retrying saved address once",
        pairing->peer.scale_id);

    vTaskDelay(pdMS_TO_TICKS(750));

    err =
        ble_client_fetch(
            &pairing->peer,
            state);

    if (err == ESP_OK &&
        ble_client_state_is_compatible(
            &pairing->peer,
            state)) {
        ESP_LOGI(
            TAG,
            "Saved-address BLE retry succeeded");
        return ESP_OK;
    }

    /*
     * The logical KegScale-XXXX identity is stable. Only after two direct
     * failures do an exact-ID scan to repair a genuinely changed BLE address.
     * Never fall back to strongest RSSI or a different scale.
     */
    ESP_LOGW(
        TAG,
        "Saved-address retry failed; looking for exact ID %s",
        pairing->peer.scale_id);

    ble_client_peer_t recovered = {0};

    err =
        find_peer_by_id(
            pairing->peer.scale_id,
            &recovered);

    if (err != ESP_OK) {
        return err;
    }

    /*
     * Give the controller a short handoff interval after active scanning.
     * This avoids immediately starting a connection on the same radio state
     * transition that completed the discovery procedure.
     */
    vTaskDelay(pdMS_TO_TICKS(250));

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
        pairing_save(
            &recovered),
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

static bool display_update_needed(
    const ble_client_scale_state_t *state)
{
    if (state == NULL ||
        !state->update.valid ||
        state->update.size_bytes == 0 ||
        strcmp(
            state->update.hardware,
            DISPLAY_OTA_HARDWARE_ID) != 0) {
        return false;
    }

    const esp_app_desc_t *app =
        esp_app_get_description();

    return app != NULL &&
        strcmp(
            state->update.version,
            app->version) != 0;
}

static void install_display_update_if_needed(
    const pairing_config_t *pairing,
    const ble_client_scale_state_t *state)
{
    if (!display_update_needed(state)) {
        return;
    }

    ESP_LOGI(
        TAG,
        "Display update %s is available; requesting encrypted Wi-Fi/OTA bundle",
        state->update.version);

    ble_client_update_bundle_t *bundle =
        calloc(
            1,
            sizeof(*bundle));

    if (bundle == NULL) {
        ESP_LOGW(
            TAG,
            "Display OTA failed: could not allocate update bundle");
        return;
    }

    esp_err_t err =
        ble_client_fetch_update_bundle(
            &pairing->peer,
            bundle);

    if (err == ESP_OK &&
        (strcmp(
             bundle->version,
             state->update.version) != 0 ||
         strcmp(
             bundle->sha256,
             state->update.sha256) != 0 ||
         bundle->size_bytes !=
             state->update.size_bytes)) {
        err = ESP_ERR_INVALID_RESPONSE;
    }

    if (err == ESP_OK) {
        err = display_ota_install(bundle);
    }

    memset(
        bundle,
        0,
        sizeof(*bundle));
    free(bundle);
    bundle = NULL;

    if (err == ESP_OK) {
        ESP_LOGI(
            TAG,
            "Display OTA staged successfully; rebooting into %s",
            state->update.version);
        vTaskDelay(pdMS_TO_TICKS(250));
        esp_restart();
    }

    ESP_LOGW(
        TAG,
        "Display OTA failed: %s; Wi-Fi is off and the current firmware remains active",
        esp_err_to_name(err));
}

static esp_err_t wait_for_touch_pour_result(
    pairing_config_t *pairing,
    ble_client_scale_state_t *state,
    bool *meaningful_change)
{
    if (pairing == NULL ||
        state == NULL ||
        meaningful_change == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    *meaningful_change = false;

    ESP_LOGI(
        TAG,
        "Touch wake: waiting %d seconds before checking the pour",
        CONFIG_KEG_DISPLAY_TOUCH_INITIAL_WAIT_SECONDS);

    vTaskDelay(
        pdMS_TO_TICKS(
            CONFIG_KEG_DISPLAY_TOUCH_INITIAL_WAIT_SECONDS *
            1000));

    const TickType_t started =
        xTaskGetTickCount();

    int observation_seconds =
        CONFIG_KEG_DISPLAY_TOUCH_MAX_WAIT_SECONDS -
        CONFIG_KEG_DISPLAY_TOUCH_INITIAL_WAIT_SECONDS;

    if (observation_seconds < 0) {
        observation_seconds = 0;
    }

    const TickType_t remaining_window =
        pdMS_TO_TICKS(
            observation_seconds *
            1000);

    while (true) {
        esp_err_t err =
            fetch_paired_state(
                pairing,
                state);

        if (err == ESP_OK) {
            if (state->unpair_requested) {
                ESP_LOGI(
                    TAG,
                    "Touch wake: authenticated unpair request received");
                return ESP_OK;
            }

            if (should_refresh(
                    &pairing->peer,
                    state)) {
                *meaningful_change = true;

                ESP_LOGI(
                    TAG,
                    "Touch wake: meaningful stable change detected; seq=%u servings=%u weight=%.3f lb",
                    (unsigned)state->sequence,
                    (unsigned)state->remaining_servings,
                    (double)state->total_weight_lbs);

                return ESP_OK;
            }

            ESP_LOGI(
                TAG,
                "Touch wake: no settled meaningful change yet; seq=%u stable=%s",
                (unsigned)state->sequence,
                (state->flags &
                 BLE_SCALE_FLAG_STABLE) ?
                    "yes" :
                    "no");
        } else {
            ESP_LOGW(
                TAG,
                "Touch wake scale check failed: %s",
                esp_err_to_name(err));
        }

        const TickType_t elapsed =
            xTaskGetTickCount() -
            started;

        if (elapsed >= remaining_window) {
            ESP_LOGI(
                TAG,
                "Touch wake observation window ended with no new settled change");
            return err;
        }

        vTaskDelay(
            pdMS_TO_TICKS(
                CONFIG_KEG_DISPLAY_TOUCH_RETRY_SECONDS *
                1000));
    }
}

static void enter_pairing_mode(void)
{
    int last_screen = -1;

    display_ui_show_message(
        "KEG DISPLAY",
        "READY TO PAIR",
        "OPEN SCALE WEBPAGE");
    last_screen = 0;

    while (true) {
        ble_client_peer_t candidates[
            BLE_CLIENT_MAX_CANDIDATES] = {0};
        size_t count = 0;

        esp_err_t err =
            scan_scales(
                candidates,
                &count);

        if (err != ESP_OK) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        ble_client_peer_t *selected = NULL;
        size_t pairing_count = 0;

        for (size_t i = 0; i < count; ++i) {
            if (!candidates[i].pairing_mode) {
                continue;
            }

            ++pairing_count;
            selected = &candidates[i];
        }

        if (pairing_count == 0) {
            if (last_screen != 0) {
                display_ui_show_message(
                    "KEG DISPLAY",
                    "READY TO PAIR",
                    "OPEN SCALE WEBPAGE");
                last_screen = 0;
            }
            continue;
        }

        if (pairing_count > 1 ||
            selected == NULL) {
            if (last_screen != 1) {
                display_ui_show_message(
                    "KEG DISPLAY",
                    "MULTIPLE SCALES",
                    "PAIR ONE SCALE ONLY");
                last_screen = 1;
            }

            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        const uint32_t passkey =
            100000U +
            (esp_random() % 900000U);

        display_ui_show_pairing_code(
            selected->scale_id,
            passkey);
        last_screen = 2;

        ESP_LOGI(
            TAG,
            "Pairing with %s using a one-time display-generated passkey",
            selected->scale_id);

        err =
            ble_client_pair(
                selected,
                passkey);

        if (err == ESP_OK) {
            ble_client_scale_state_t state = {0};

            err =
                validate_and_save_peer(
                    selected,
                    &state);

            if (err == ESP_OK) {
                display_ui_show_message(
                    "PAIR DISPLAY",
                    "SUCCESSFULLY",
                    selected->scale_id);

                vTaskDelay(pdMS_TO_TICKS(1000));

                render_if_needed(
                    selected,
                    &state);

                go_to_sleep();
            }
        }

        ble_client_forget_peer(selected);
        pairing_clear();

        ESP_LOGW(
            TAG,
            "Display pairing attempt failed: %s",
            esp_err_to_name(err));

        display_ui_show_message(
            "PAIR DISPLAY",
            "PAIRING FAILED",
            "TRY ADD DISPLAY AGAIN");

        last_screen = 3;
        vTaskDelay(pdMS_TO_TICKS(1500));
    }
}

static bool handle_unpair_request(
    pairing_config_t *pairing,
    const ble_client_scale_state_t *state)
{
    if (pairing == NULL ||
        state == NULL ||
        !state->unpair_requested) {
        return false;
    }

    ESP_LOGI(
        TAG,
        "Authenticated scale requested display unpair");

    ble_client_forget_peer(
        &pairing->peer);
    pairing_clear();

    memset(
        &s_retained,
        0,
        sizeof(s_retained));

    display_ui_show_message(
        "KEG DISPLAY",
        "DISPLAY UNPAIRED",
        "READY TO PAIR");

    vTaskDelay(pdMS_TO_TICKS(750));
    enter_pairing_mode();
    return true;
}

void app_main(void)
{
    ESP_LOGI(
        TAG,
        "KegScaleESPDisplay starting; wake=%s",
        wake_reason());

    init_nvs();

    pairing_config_t pairing = {0};
    esp_err_t err =
        pairing_load(&pairing);

    const bool touch_wake =
        is_touch_wake();

    ESP_ERROR_CHECK(
        ble_client_init());

    esp_err_t confirm_err =
        display_ota_confirm_running_image();

    if (confirm_err != ESP_OK) {
        ESP_LOGW(
            TAG,
            "Could not confirm running OTA image: %s",
            esp_err_to_name(confirm_err));
    }

    const uint32_t wake_causes =
        esp_sleep_get_wakeup_causes();

    if (wake_causes == 0) {
        uint8_t power_cycles = 0;
        bool recovery_requested = false;

        esp_err_t recovery_err =
            pairing_note_power_cycle(
                &power_cycles,
                &recovery_requested);

        if (recovery_err == ESP_OK) {
            ESP_LOGI(
                TAG,
                "Power-cycle recovery count=%u",
                (unsigned)power_cycles);
        }

        if (recovery_err == ESP_OK &&
            recovery_requested) {
            ESP_LOGW(
                TAG,
                "Three deliberate power cycles detected; entering recovery pairing mode");

            if (pairing.paired) {
                ble_client_forget_peer(
                    &pairing.peer);
            }

            pairing_clear();
            memset(
                &pairing,
                0,
                sizeof(pairing));
            memset(
                &s_retained,
                0,
                sizeof(s_retained));

            display_ui_show_message(
                "KEG DISPLAY",
                "RECOVERY MODE",
                "READY TO PAIR");

            vTaskDelay(pdMS_TO_TICKS(750));
        }
    } else {
        pairing_reset_power_cycle_count();
    }

    if (err == ESP_OK &&
        pairing.paired) {
        ble_client_scale_state_t state = {0};

#if CONFIG_KEG_DISPLAY_TOUCH_WAKE
        if (touch_wake) {
            bool meaningful_change = false;

            err =
                wait_for_touch_pour_result(
                    &pairing,
                    &state,
                    &meaningful_change);

            if (err == ESP_OK) {
                handle_unpair_request(
                    &pairing,
                    &state);
            }

            if (err == ESP_OK &&
                meaningful_change) {
                render_if_needed(
                    &pairing.peer,
                    &state);
            } else if (err != ESP_OK) {
                ESP_LOGW(
                    TAG,
                    "Touch wake ended without a successful final scale read: %s",
                    esp_err_to_name(err));
            }

            /*
             * Touch wake already has a fresh authenticated scale state.
             * Process a pending display OTA here too so a user can wake the
             * display to install an update instead of waiting for the next
             * timer wake.
             */
            if (err == ESP_OK) {
                install_display_update_if_needed(
                    &pairing,
                    &state);
            }

            go_to_sleep();
        }
#endif

        err =
            fetch_paired_state(
                &pairing,
                &state);

        if (err == ESP_OK) {
            handle_unpair_request(
                &pairing,
                &state);

            render_if_needed(
                &pairing.peer,
                &state);

            install_display_update_if_needed(
                &pairing,
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

    enter_pairing_mode();
}
