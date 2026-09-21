#include <ctype.h>
#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ble_client.h"
#include "display_ota.h"
#include "ota_authorization_policy.h"
#include "display_ui.h"
#include "driver/gpio.h"
#include "esp_adc/adc_oneshot.h"
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
#include "nvs.h"
#include "nvs_flash.h"
#include "pairing.h"
#include "sdkconfig.h"
#include "touch_wake.h"
#include "power_policy.h"

static const char *TAG = "display";

/* Bumped when retained scale-offline tracking was added. */
#define RETAINED_MAGIC 0x4B534454U
#define SIGNIFICANT_WEIGHT_LBS 0.5f
#define SCALE_OFFLINE_FAILURE_THRESHOLD 5U
#define BATTERY_ADC_SAMPLES 16
#define BATTERY_ADC_FULL_SCALE 4095.0f
#define BATTERY_DIVIDER_SCALE 7.46f
#define DISPLAY_STATE_NAMESPACE "display_state"
#define KEY_TOUCH_CAL_PENDING "touch_cal"
#define KEY_OTA_SCREEN_PENDING "ota_screen"

typedef struct {
    uint32_t magic;
    char scale_id[BLE_CLIENT_SCALE_ID_MAX + 1];
    uint16_t sequence;
    uint8_t profile_revision;
    uint8_t display_config_revision;
    uint8_t touch_threshold_percent;
    uint8_t periodic_checkin_disabled;
    uint8_t battery_percent;
    uint8_t consecutive_scale_failures;
    uint8_t scale_offline_displayed;
    uint16_t remaining_servings;
    float total_weight_lbs;
} retained_state_t;

RTC_DATA_ATTR static retained_state_t s_retained;

static uint8_t s_touch_threshold_percent =
    CONFIG_KEG_DISPLAY_TOUCH_THRESHOLD_PERCENT;
static bool s_periodic_checkin_enabled = false;
/* Separate from the last rendered state, which is reset after a redraw. */
RTC_DATA_ATTR static uint8_t s_touch_phase;
RTC_DATA_ATTR static bool s_touch_ack_pending;
static bool s_lightweight_fetch;
static void disable_wake_source_if_enabled(esp_sleep_source_t source);

static esp_err_t set_display_state_flag(
    const char *key,
    bool enabled)
{
    nvs_handle_t nvs;
    esp_err_t err =
        nvs_open(
            DISPLAY_STATE_NAMESPACE,
            NVS_READWRITE,
            &nvs);

    if (err != ESP_OK) {
        return err;
    }

    if (enabled) {
        err = nvs_set_u8(
            nvs,
            key,
            1);
    } else {
        err = nvs_erase_key(
            nvs,
            key);

        if (err == ESP_ERR_NVS_NOT_FOUND) {
            err = ESP_OK;
        }
    }

    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }

    nvs_close(nvs);
    return err;
}

static bool display_state_flag_is_set(
    const char *key)
{
    nvs_handle_t nvs;
    esp_err_t err =
        nvs_open(
            DISPLAY_STATE_NAMESPACE,
            NVS_READONLY,
            &nvs);

    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return false;
    }

    if (err != ESP_OK) {
        ESP_LOGW(
            TAG,
            "Could not read display state flag %s: %s",
            key,
            esp_err_to_name(err));
        return false;
    }

    uint8_t pending = 0;
    err = nvs_get_u8(
        nvs,
        key,
        &pending);
    nvs_close(nvs);

    if (err != ESP_OK &&
        err != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(
            TAG,
            "Could not load display state flag %s: %s",
            key,
            esp_err_to_name(err));
    }

    return err == ESP_OK && pending == 1;
}

static void sleep_for_touch_delay(unsigned seconds)
{
    pairing_reset_power_cycle_count();
    disable_wake_source_if_enabled(ESP_SLEEP_WAKEUP_ALL);
    ESP_ERROR_CHECK(esp_sleep_enable_timer_wakeup((uint64_t)seconds * 1000000ULL));
    ESP_LOGI(TAG, "Touch phase %u: deep sleep for %u seconds",
             (unsigned)s_touch_phase, seconds);
    /* Timer-only during the pour delay: a held finger cannot cause a wake loop. */
    fflush(stdout);
    esp_deep_sleep_start();
}

static bool touch_threshold_is_valid(
    uint8_t threshold_percent)
{
    return
        threshold_percent >=
            TOUCH_WAKE_THRESHOLD_MIN_PERCENT &&
        threshold_percent <=
            TOUCH_WAKE_THRESHOLD_MAX_PERCENT;
}

static void apply_display_settings(
    const ble_client_scale_state_t *state)
{
    if (state == NULL) {
        return;
    }

    if (touch_threshold_is_valid(
            state->touch_threshold_percent)) {
        s_touch_threshold_percent =
            state->touch_threshold_percent;
    }

    if ((state->display_flags &
         BLE_DISPLAY_FLAG_CONFIG_PRESENT) != 0) {
        s_periodic_checkin_enabled =
            (state->display_flags &
             BLE_DISPLAY_FLAG_DISABLE_PERIODIC_CHECKIN) == 0;
    }
}

static uint8_t quantize_battery_percent(float percent)
{
    if (!isfinite(percent) || percent <= 10.0f) {
        return 0;
    }

    if (percent <= 30.0f) {
        return 20;
    }

    if (percent <= 50.0f) {
        return 40;
    }

    if (percent <= 70.0f) {
        return 60;
    }

    if (percent <= 90.0f) {
        return 80;
    }

    return 100;
}

static uint8_t battery_fallback_percent(void)
{
    if (s_retained.magic == RETAINED_MAGIC &&
        s_retained.battery_percent <= 100) {
        return s_retained.battery_percent;
    }

    return 100;
}

static uint8_t read_battery_percent(void)
{
    ble_client_set_display_battery_millivolts(0);

    adc_oneshot_unit_handle_t adc = NULL;
    const adc_oneshot_unit_init_cfg_t unit_config = {
        .unit_id = ADC_UNIT_1,
    };

    esp_err_t err =
        adc_oneshot_new_unit(
            &unit_config,
            &adc);

    if (err != ESP_OK) {
        ESP_LOGW(
            TAG,
            "Battery ADC init failed: %s",
            esp_err_to_name(err));
        return battery_fallback_percent();
    }

    const adc_oneshot_chan_cfg_t channel_config = {
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_12,
    };

    err =
        adc_oneshot_config_channel(
            adc,
            ADC_CHANNEL_7,
            &channel_config);

    int64_t raw_total = 0;
    int samples = 0;

    if (err == ESP_OK) {
        for (int i = 0;
             i < BATTERY_ADC_SAMPLES;
             ++i) {
            int raw = 0;
            if (adc_oneshot_read(
                    adc,
                    ADC_CHANNEL_7,
                    &raw) == ESP_OK) {
                raw_total += raw;
                ++samples;
            }
        }
    }

    adc_oneshot_del_unit(adc);

    if (err != ESP_OK || samples == 0) {
        ESP_LOGW(
            TAG,
            "Battery ADC read failed: %s",
            esp_err_to_name(err));
        return battery_fallback_percent();
    }

    const float raw_average =
        (float)raw_total / (float)samples;
    const float voltage =
        (raw_average / BATTERY_ADC_FULL_SCALE) *
        BATTERY_DIVIDER_SCALE;

    const uint16_t battery_millivolts =
        (uint16_t)(voltage * 1000.0f + 0.5f);
    ble_client_set_display_battery_millivolts(
        battery_millivolts);

    float percent = 0.0f;

    if (voltage >= 4.20f) {
        percent = 100.0f;
    } else if (voltage <= 3.50f) {
        percent = 0.0f;
    } else {
        const float v2 = voltage * voltage;
        const float v3 = v2 * voltage;
        const float v4 = v3 * voltage;

        percent =
            2836.9625f * v4 -
            43987.4889f * v3 +
            255233.8134f * v2 -
            656689.7123f * voltage +
            632041.7303f;

        if (percent < 0.0f) {
            percent = 0.0f;
        } else if (percent > 100.0f) {
            percent = 100.0f;
        }
    }

    const uint8_t quantized =
        quantize_battery_percent(percent);

    ESP_LOGI(
        TAG,
        "Battery ADC raw=%.0f voltage=%.2fV estimated=%.0f%% display=%u%%",
        (double)raw_average,
        (double)voltage,
        (double)percent,
        (unsigned)quantized);

    return quantized;
}

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
        peer != NULL &&
        s_retained.magic == RETAINED_MAGIC &&
        strcmp(
            s_retained.scale_id,
            peer->scale_id) == 0;
}

static void initialize_retained_peer(
    const ble_client_peer_t *peer)
{
    if (peer == NULL ||
        retained_matches_peer(peer)) {
        return;
    }

    memset(
        &s_retained,
        0,
        sizeof(s_retained));

    s_retained.magic = RETAINED_MAGIC;
    s_retained.battery_percent = 255U;

    strlcpy(
        s_retained.scale_id,
        peer->scale_id,
        sizeof(s_retained.scale_id));
}

static void remember_runtime_settings(
    const ble_client_peer_t *peer)
{
    if (!retained_matches_peer(peer)) {
        return;
    }

    if (s_retained.consecutive_scale_failures > 0) {
        ESP_LOGI(
            TAG,
            "Scale %s reachable again after %u failed check(s)",
            peer->scale_id,
            (unsigned)s_retained.consecutive_scale_failures);
    }

    s_retained.touch_threshold_percent =
        s_touch_threshold_percent;
    s_retained.periodic_checkin_disabled =
        s_periodic_checkin_enabled ? 0U : 1U;
    s_retained.consecutive_scale_failures = 0;
}

static bool record_scale_check_failure(
    const ble_client_peer_t *peer)
{
    if (peer == NULL) {
        return false;
    }

    initialize_retained_peer(peer);

    if (s_retained.consecutive_scale_failures < 255U) {
        ++s_retained.consecutive_scale_failures;
    }

    ESP_LOGW(
        TAG,
        "Scale %s consecutive failed check-ins=%u; offline screen after more than %u failures",
        peer->scale_id,
        (unsigned)s_retained.consecutive_scale_failures,
        (unsigned)SCALE_OFFLINE_FAILURE_THRESHOLD);

    if (s_retained.consecutive_scale_failures <=
            SCALE_OFFLINE_FAILURE_THRESHOLD ||
        s_retained.scale_offline_displayed != 0) {
        return false;
    }

    esp_err_t err =
        display_ui_show_message(
            "KEG DISPLAY",
            "SCALE OFFLINE",
            peer->scale_id);

    if (err != ESP_OK) {
        ESP_LOGW(
            TAG,
            "Could not show Scale Offline screen: %s",
            esp_err_to_name(err));
        return false;
    }

    s_retained.scale_offline_displayed = 1U;

    ESP_LOGW(
        TAG,
        "Scale %s marked offline after %u consecutive failed check-ins",
        peer->scale_id,
        (unsigned)s_retained.consecutive_scale_failures);

    return true;
}

static bool should_refresh(
    const ble_client_peer_t *peer,
    const ble_client_scale_state_t *state,
    uint8_t battery_percent)
{
    if (!retained_matches_peer(peer)) {
        return true;
    }

    /* Restore live scale data immediately after an offline screen. */
    if (s_retained.scale_offline_displayed != 0) {
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
            s_retained.remaining_servings ||
        battery_percent !=
            s_retained.battery_percent) {
        return true;
    }

    return fabsf(
               state->total_weight_lbs -
               s_retained.total_weight_lbs) >=
        SIGNIFICANT_WEIGHT_LBS;
}

static void remember_displayed_state(
    const ble_client_peer_t *peer,
    const ble_client_scale_state_t *state,
    uint8_t battery_percent)
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

    s_retained.touch_threshold_percent =
        s_touch_threshold_percent;

    s_retained.periodic_checkin_disabled =
        s_periodic_checkin_enabled ? 0U : 1U;

    s_retained.battery_percent =
        battery_percent;

    s_retained.remaining_servings =
        state->remaining_servings;

    s_retained.total_weight_lbs =
        state->total_weight_lbs;
}

static void disable_wake_source_if_enabled(
    esp_sleep_source_t source)
{
    const esp_err_t err =
        esp_sleep_disable_wakeup_source(source);

    if (err != ESP_OK &&
        err != ESP_ERR_INVALID_STATE) {
        ESP_ERROR_CHECK(err);
    }
}

static void configure_wake_sources(void)
{
    /*
     * Wake-source configuration survives a sleep cycle. Clear every previous
     * source first, then arm only the sources selected below. When frequent
     * check-in is disabled, the one-hour safety timer remains armed so a touch
     * sensor problem cannot leave the display permanently unreachable.
     */
    disable_wake_source_if_enabled(
        ESP_SLEEP_WAKEUP_ALL);

    const uint64_t wake_seconds = power_next_check_seconds(
        s_periodic_checkin_enabled, s_retained.consecutive_scale_failures,
        CONFIG_KEG_DISPLAY_SLEEP_SECONDS, CONFIG_KEG_DISPLAY_SAFETY_WAKE_SECONDS);
    ESP_LOGI(TAG, "Next scheduled check in %llu seconds", (unsigned long long)wake_seconds);

    ESP_ERROR_CHECK(
        esp_sleep_enable_timer_wakeup(
            wake_seconds * 1000000ULL));

#if CONFIG_KEG_DISPLAY_TOUCH_WAKE
    ESP_ERROR_CHECK(
        touch_wake_prepare(
            s_touch_threshold_percent));
#endif
}

static void go_to_sleep(void)
{
    pairing_reset_power_cycle_count();
    configure_wake_sources();

#if CONFIG_KEG_DISPLAY_TOUCH_WAKE
    if (s_periodic_checkin_enabled) {
        ESP_LOGI(
            TAG,
            "Sleeping for %d seconds; GPIO12 capacitive touch also wakes the display",
            CONFIG_KEG_DISPLAY_SLEEP_SECONDS);
    } else {
        ESP_LOGI(
            TAG,
            "Frequent check-in disabled; safety check in %d seconds; GPIO12 capacitive touch also wakes the display",
            CONFIG_KEG_DISPLAY_SAFETY_WAKE_SECONDS);
    }
#else
    ESP_LOGI(
        TAG,
        "Sleeping for %d seconds",
        s_periodic_checkin_enabled ?
            CONFIG_KEG_DISPLAY_SLEEP_SECONDS :
            CONFIG_KEG_DISPLAY_SAFETY_WAKE_SECONDS);
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

    apply_display_settings(state);
    return pairing_save(peer);
}

static esp_err_t fetch_paired_state_once(
    pairing_config_t *pairing,
    ble_client_scale_state_t *state)
{
    esp_err_t err = ble_client_fetch_mode(&pairing->peer, state, !s_lightweight_fetch);
    if (err != ESP_OK) return err;
    if (!ble_client_state_is_compatible(&pairing->peer, state)) {
        return ESP_ERR_INVALID_VERSION;
    }
    apply_display_settings(state);
    remember_runtime_settings(&pairing->peer);
    return ESP_OK;
}

static bool render_if_needed(
    const ble_client_peer_t *peer,
    const ble_client_scale_state_t *state,
    uint8_t battery_percent)
{
    if (!state->force_refresh_requested &&
        !should_refresh(
            peer,
            state,
            battery_percent)) {
        ESP_LOGI(
            TAG,
            "No meaningful display change; keeping existing e-paper image");
        return false;
    }

    apply_display_settings(state);

    esp_err_t err =
        display_ui_show_scale(
            peer,
            state,
            battery_percent,
            state->force_refresh_requested);

    if (err == ESP_OK) {
        remember_displayed_state(
            peer,
            state,
            battery_percent);
        return true;
    } else {
        ESP_LOGW(
            TAG,
            "Display refresh failed: %s",
            esp_err_to_name(err));
    }

    return false;
}

typedef struct {
    touch_calibration_stage_t stage;
} calibration_ui_context_t;

static esp_err_t show_touch_calibration_progress(
    touch_calibration_stage_t stage,
    uint8_t completed,
    uint8_t total,
    void *context)
{
    calibration_ui_context_t *ui = context;
    ui->stage = stage;
    ESP_LOGI(TAG, "Touch setup stage=%u completed=%u/%u",
             (unsigned)stage, (unsigned)completed, (unsigned)total);
    switch (stage) {
        case TOUCH_CALIBRATION_STAGE_BASELINE:
            return display_ui_show_calibration(
                "TOUCH SETUP - 1 OF 4", "DO NOT TOUCH",
                "THE TAP HANDLE", "MEASURING - PLEASE WAIT", 1, false);
        case TOUCH_CALIBRATION_STAGE_TOUCH_AND_HOLD:
            return display_ui_show_calibration(
                "TOUCH SETUP - 2 OF 4", "TOUCH AND HOLD",
                "OUTSIDE EDGES", "HOLD UNTIL ASKED TO RELEASE", 2, false);
        case TOUCH_CALIBRATION_STAGE_RELEASE:
            return display_ui_show_calibration(
                "TOUCH SETUP - 3 OF 4", "RELEASE NOW",
                "LET GO OF THE HANDLE", "WAIT FOR THE TOUCH CHECK", 3, false);
        case TOUCH_CALIBRATION_STAGE_VERIFY:
        case TOUCH_CALIBRATION_STAGE_VERIFY_RELEASE: {
            char heading[32];
            snprintf(heading, sizeof(heading), "TOUCH CHECK %u OF %u",
                     (unsigned)completed + 1U, (unsigned)total);
            const bool release = stage == TOUCH_CALIBRATION_STAGE_VERIFY_RELEASE;
            return display_ui_show_calibration(
                heading, release ? "RELEASE NOW" : "TOUCH AND HOLD",
                release ? "TOUCH DETECTED" : "OUTSIDE EDGES",
                release ? "LET GO TO FINISH THIS CHECK" : "HOLD UNTIL ASKED TO RELEASE",
                4, false);
        }
        default:
            return ESP_ERR_INVALID_ARG;
    }
}

static const char *touch_calibration_error_text(
    touch_calibration_failure_t failure)
{
    switch (failure) {
        case TOUCH_CALIBRATION_FAILURE_TIMEOUT:
            return "NO TOUCH DETECTED";
        case TOUCH_CALIBRATION_FAILURE_NOISY:
            return "SIGNAL TOO NOISY";
        case TOUCH_CALIBRATION_FAILURE_WEAK_SIGNAL:
            return "TOUCH TOO WEAK";
        case TOUCH_CALIBRATION_FAILURE_HARDWARE:
            return "SENSOR ERROR";
        default:
            return "PLEASE TRY AGAIN";
    }
}

static void run_touch_calibration(
    const ble_client_peer_t *peer,
    ble_client_scale_state_t *state,
    uint8_t battery_percent,
    bool resume_interrupted)
{
    touch_calibration_result_t result = {0};
    calibration_ui_context_t ui = { .stage = TOUCH_CALIBRATION_STAGE_BASELINE };

    ESP_LOGI(
        TAG,
        "%s guided touch calibration requested by scale",
        resume_interrupted ? "Resuming" : "Starting");

    esp_err_t err = ESP_OK;

    if (!resume_interrupted) {
        err = set_display_state_flag(
            KEY_TOUCH_CAL_PENDING,
            true);

        if (err != ESP_OK) {
            ESP_LOGE(
                TAG,
                "Could not persist touch calibration state: %s",
                esp_err_to_name(err));
            display_ui_show_calibration(
                "TOUCH SETUP", "COULD NOT START", "STATE SAVE ERROR",
                "START AGAIN ON SCALE PAGE", 0, true);
            vTaskDelay(pdMS_TO_TICKS(4000));
            state->force_refresh_requested = true;
            render_if_needed(
                peer,
                state,
                battery_percent);
            return;
        }
    }

    /* USB presence is not measured on this board. Give the user explicit
     * preparation time, including when a USB-disconnect reset resumes setup. */
    err = display_ui_show_calibration(
        "TOUCH SETUP", "UNPLUG USB", "USE BATTERY POWER",
        "SET DOWN - STARTS IN 10 SEC", 0, true);
    if (err == ESP_OK) {
        vTaskDelay(pdMS_TO_TICKS(10000));
        err = touch_wake_calibrate(
            show_touch_calibration_progress, &ui, &result);
    } else {
        result.failure = TOUCH_CALIBRATION_FAILURE_HARDWARE;
    }

    if (err == ESP_OK) {
        err =
            ble_client_save_touch_threshold(
                peer,
                result.threshold_percent);
    }

    esp_err_t clear_err =
        set_display_state_flag(
            KEY_TOUCH_CAL_PENDING,
            false);

    if (clear_err != ESP_OK) {
        ESP_LOGW(
            TAG,
            "Could not clear touch calibration state: %s",
            esp_err_to_name(clear_err));
    }

    if (err == ESP_OK) {
        s_touch_threshold_percent =
            result.threshold_percent;
        state->touch_threshold_percent =
            result.threshold_percent;

        display_ui_show_calibration(
            "TOUCH SETUP COMPLETE", "TOUCH READY", "3 CHECKS PASSED",
            "SAVED - RETURNING TO SCALE", 4, false);

        ESP_LOGI(
            TAG,
            "Touch calibration passed: baseline=%" PRIu32
            " touched=%" PRIu32 " noise=%" PRIu32
            " threshold=%u%%",
            result.untouched_value,
            result.touched_value,
            result.noise_span,
            (unsigned)result.threshold_percent);
    } else {
        const char *reason = touch_calibration_error_text(result.failure);
        if (result.failure == TOUCH_CALIBRATION_FAILURE_NONE)
            reason = "COULD NOT SAVE TO SCALE";
        else if (result.failure == TOUCH_CALIBRATION_FAILURE_TIMEOUT &&
                 (ui.stage == TOUCH_CALIBRATION_STAGE_RELEASE ||
                  ui.stage == TOUCH_CALIBRATION_STAGE_VERIFY_RELEASE))
            reason = "RELEASE NOT DETECTED";
        esp_err_t screen_err = display_ui_show_calibration(
            "TOUCH SETUP NOT SAVED", "TRY AGAIN", reason,
            "RESTART FROM SCALE PAGE", 0, false);
        if (screen_err != ESP_OK) {
            ESP_LOGE(TAG, "Calibration failure screen failed: %s",
                     esp_err_to_name(screen_err));
        }

        ESP_LOGW(
            TAG,
            "Touch calibration failed: %s (reason=%u baseline=%" PRIu32
            " touched=%" PRIu32 " noise=%" PRIu32 ")",
            esp_err_to_name(err),
            (unsigned)result.failure,
            result.untouched_value,
            result.touched_value,
            result.noise_span);
    }

    /* Keep the error readable on battery without requiring USB diagnostics. */
    vTaskDelay(pdMS_TO_TICKS(err == ESP_OK ? 4000 : 30000));

    state->force_refresh_requested = true;
    render_if_needed(
        peer,
        state,
        battery_percent);
}

static void clear_touch_acknowledgement_if_needed(
    bool touch_ack_visible,
    bool screen_refreshed)
{
    if (!touch_ack_visible ||
        screen_refreshed) {
        return;
    }

    esp_err_t err =
        display_ui_clear_touch_acknowledged();

    if (err != ESP_OK) {
        ESP_LOGW(
            TAG,
            "Could not clear the touch acknowledgement: %s",
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
    ble_client_scale_state_t *state,
    uint8_t battery_percent)
{
    if (!display_update_needed(state)) {
        return;
    }

    ESP_LOGI(
        TAG,
        "Display update %s is advertised; checking OTA authorization",
        state->update.version);

    /*
     * IMPORTANT:
     * Update metadata means only that a newer image exists. It does NOT mean
     * the update has been approved.
     *
     * The protected Wi-Fi/OTA bundle is only readable after the scale has
     * approved the display update. Check for that bundle before changing the
     * e-paper screen or starting an OTA.
     */
    ble_client_update_bundle_t *bundle =
        calloc(
            1,
            sizeof(*bundle));

    if (bundle == NULL) {
        ESP_LOGW(
            TAG,
            "Display OTA authorization check deferred: out of memory");
        return;
    }

    esp_err_t err =
        ble_client_fetch_update_bundle(
            &pairing->peer,
            bundle);

    const display_ota_authorization_result_t authorization =
        display_ota_authorization_evaluate(
            err,
            &state->update,
            bundle);

    if (authorization ==
        DISPLAY_OTA_AUTHORIZATION_NOT_APPROVED) {
        ESP_LOGI(
            TAG,
            "Display update %s is available but not approved; staying on current firmware",
            state->update.version);

        memset(
            bundle,
            0,
            sizeof(*bundle));
        free(bundle);
        return;
    }

    if (authorization !=
        DISPLAY_OTA_AUTHORIZATION_APPROVED) {
        ESP_LOGW(
            TAG,
            "Approved OTA bundle does not match advertised display update %s; install skipped",
            state->update.version);

        memset(
            bundle,
            0,
            sizeof(*bundle));
        free(bundle);
        return;
    }

    ESP_LOGI(
        TAG,
        "Display update %s is approved; starting OTA",
        state->update.version);

    esp_err_t screen_state_err =
        set_display_state_flag(
            KEY_OTA_SCREEN_PENDING,
            true);

    if (screen_state_err != ESP_OK) {
        ESP_LOGW(
            TAG,
            "Could not persist OTA screen state: %s",
            esp_err_to_name(screen_state_err));
    }

    esp_err_t screen_err =
        display_ui_show_message(
            "DISPLAY UPDATE",
            "UPDATE IN PROGRESS",
            "PLEASE WAIT");

    if (screen_err != ESP_OK) {
        ESP_LOGW(
            TAG,
            "Could not show OTA progress screen: %s",
            esp_err_to_name(screen_err));
    }

    err = display_ota_install(bundle);

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
        "Display OTA failed after approval: %s; current firmware remains active",
        esp_err_to_name(err));

    display_ui_show_message(
        "DISPLAY UPDATE",
        "UPDATE FAILED",
        "WILL RETRY LATER");
    vTaskDelay(pdMS_TO_TICKS(3000));

    screen_state_err =
        set_display_state_flag(
            KEY_OTA_SCREEN_PENDING,
            false);

    if (screen_state_err != ESP_OK) {
        ESP_LOGW(
            TAG,
            "Could not clear OTA screen state: %s",
            esp_err_to_name(screen_state_err));
    }

    state->force_refresh_requested = true;
    render_if_needed(
        &pairing->peer,
        state,
        battery_percent);
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
        ble_client_peer_t *setup_scale = NULL;
        size_t setup_count = 0;

        for (size_t i = 0; i < count; ++i) {
            if (candidates[i].setup_url_available) {
                ++setup_count;
                setup_scale = &candidates[i];
            }

            if (!candidates[i].pairing_mode) {
                continue;
            }

            ++pairing_count;
            selected = &candidates[i];
        }

        if (pairing_count == 0) {
            if (setup_count == 1 &&
                setup_scale != NULL) {
                if (last_screen != 4) {
                    display_ui_show_setup_qr(
                        setup_scale->scale_id,
                        setup_scale->ip_address);
                    last_screen = 4;
                }
            } else if (setup_count > 1) {
                if (last_screen != 1) {
                    display_ui_show_message(
                        "KEG DISPLAY",
                        "MULTIPLE SCALES",
                        "POWER ON ONE SCALE");
                    last_screen = 1;
                }
            } else if (last_screen != 0) {
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

            const uint8_t battery_percent =
                read_battery_percent();

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
                    &state,
                    battery_percent);

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

    const bool touch_calibration_pending =
        display_state_flag_is_set(
            KEY_TOUCH_CAL_PENDING);
    const bool ota_screen_pending =
        display_state_flag_is_set(
            KEY_OTA_SCREEN_PENDING);

    if (touch_calibration_pending) {
        ESP_LOGI(
            TAG,
            "Touch calibration was interrupted; will resume at step 1");
    }

    if (ota_screen_pending) {
        ESP_LOGI(
            TAG,
            "OTA screen is pending restoration after update reboot");
    }

    pairing_config_t pairing = {0};
    esp_err_t err =
        pairing_load(&pairing);

    const bool touch_wake =
        is_touch_wake();
    const bool timer_wake =
        (esp_sleep_get_wakeup_causes() & BIT(ESP_SLEEP_WAKEUP_TIMER)) != 0;
    if (!timer_wake) {
        s_touch_phase = 0;
        s_touch_ack_pending = false;
    }
    bool touch_ack_visible = timer_wake && s_touch_ack_pending;

    if (touch_wake &&
        !touch_calibration_pending &&
        err == ESP_OK &&
        pairing.paired) {
        touch_ack_visible =
            display_ui_show_touch_acknowledged() ==
            ESP_OK;

        if (!touch_ack_visible) {
            ESP_LOGW(
                TAG,
                "Could not draw the touch acknowledgement");
        }
    }

    if (touch_wake &&
        !touch_calibration_pending &&
        err == ESP_OK &&
        pairing.paired) {
        s_touch_phase = 1;
        s_touch_ack_pending = touch_ack_visible;
        sleep_for_touch_delay(CONFIG_KEG_DISPLAY_TOUCH_INITIAL_WAIT_SECONDS);
    }
    const bool touch_result_wake = timer_wake && s_touch_phase != 0;
    s_lightweight_fetch = touch_result_wake;

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

    if (wake_causes == 0 &&
        !touch_calibration_pending) {
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
        if (retained_matches_peer(
                &pairing.peer)) {
            if (touch_threshold_is_valid(
                    s_retained.touch_threshold_percent)) {
                s_touch_threshold_percent =
                    s_retained.touch_threshold_percent;
            }

            s_periodic_checkin_enabled =
                s_retained.periodic_checkin_disabled == 0;
        }

        const uint8_t battery_percent =
            read_battery_percent();

        ble_client_scale_state_t state = {0};

        if (touch_result_wake) {
            err = fetch_paired_state_once(&pairing, &state);
            bool screen_refreshed = false;
            if (err == ESP_OK) {
                handle_unpair_request(&pairing, &state);
                if (touch_calibration_pending ||
                    state.touch_calibration_requested) {
                    run_touch_calibration(
                        &pairing.peer,
                        &state,
                        battery_percent,
                        touch_calibration_pending);
                    s_touch_phase = 0;
                    s_touch_ack_pending = false;
                    go_to_sleep();
                }
                if (power_retry_touch(s_touch_phase,
                    (state.flags & BLE_SCALE_FLAG_CALIBRATED) != 0,
                    (state.flags & BLE_SCALE_FLAG_STABLE) != 0,
                    state.force_refresh_requested)) {
                    s_touch_phase = 2;
                    sleep_for_touch_delay(CONFIG_KEG_DISPLAY_TOUCH_RETRY_SECONDS);
                }
                if (ota_screen_pending) {
                    state.force_refresh_requested = true;
                }
                screen_refreshed = render_if_needed(&pairing.peer, &state, battery_percent);
                if (ota_screen_pending &&
                    screen_refreshed) {
                    esp_err_t clear_err =
                        set_display_state_flag(
                            KEY_OTA_SCREEN_PENDING,
                            false);
                    if (clear_err != ESP_OK) {
                        ESP_LOGW(
                            TAG,
                            "Could not clear restored OTA screen state: %s",
                            esp_err_to_name(clear_err));
                    } else {
                        ESP_LOGI(
                            TAG,
                            "Restored normal display after OTA");
                    }
                }
            } else {
                screen_refreshed = record_scale_check_failure(&pairing.peer);
            }
            clear_touch_acknowledgement_if_needed(touch_ack_visible, screen_refreshed);
            s_touch_phase = 0;
            s_touch_ack_pending = false;
            go_to_sleep();
        }

        err =
            fetch_paired_state_once(
                &pairing,
                &state);

        if (err == ESP_OK) {
            handle_unpair_request(
                &pairing,
                &state);

            if (touch_calibration_pending ||
                state.touch_calibration_requested) {
                run_touch_calibration(
                    &pairing.peer,
                    &state,
                    battery_percent,
                    touch_calibration_pending);
                go_to_sleep();
            }

            if (ota_screen_pending) {
                state.force_refresh_requested = true;
            }

            const bool screen_refreshed = render_if_needed(
                &pairing.peer,
                &state,
                battery_percent);

            if (ota_screen_pending &&
                screen_refreshed) {
                esp_err_t clear_err =
                    set_display_state_flag(
                        KEY_OTA_SCREEN_PENDING,
                        false);
                if (clear_err != ESP_OK) {
                    ESP_LOGW(
                        TAG,
                        "Could not clear restored OTA screen state: %s",
                        esp_err_to_name(clear_err));
                } else {
                    ESP_LOGI(
                        TAG,
                        "Restored normal display after OTA");
                }
            }

            install_display_update_if_needed(
                &pairing,
                &state,
                battery_percent);
        } else {
            ESP_LOGW(
                TAG,
                "Paired scale %s unavailable: %s",
                pairing.peer.scale_id,
                esp_err_to_name(err));

            record_scale_check_failure(
                &pairing.peer);
        }

        go_to_sleep();
    }

    enter_pairing_mode();
}
