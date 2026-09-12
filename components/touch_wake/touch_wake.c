#include "touch_wake.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#include "driver/touch_sens.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"
#include "soc/soc_caps.h"

static const char *TAG = "touch_wake";

#define TOUCH_SAMPLE_COUNT 1
#define TOUCH_INITIAL_SCANS 3
#define TOUCH_CALIBRATION_BASELINE_SAMPLES 32
#define TOUCH_CALIBRATION_TOUCHED_SAMPLES 16
#define TOUCH_CALIBRATION_VERIFY_TAPS 3
#define TOUCH_CALIBRATION_SETTLE_MS 1500
#define TOUCH_CALIBRATION_SAMPLE_MS 50
#define TOUCH_CALIBRATION_ACTION_TIMEOUT_MS 60000

typedef struct {
    touch_sensor_handle_t sensor;
    touch_channel_handle_t channel;
    touch_channel_config_t channel_cfg;
    bool enabled;
    bool scanning;
} touch_context_t;

static esp_err_t touch_context_create(
    touch_context_t *context)
{
    if (context == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(context, 0, sizeof(*context));

    touch_sensor_sample_config_t sample_cfg[
        TOUCH_SAMPLE_COUNT] = {
        TOUCH_SENSOR_V1_DEFAULT_SAMPLE_CONFIG(
            5.0,
            TOUCH_VOLT_LIM_L_0V5,
            TOUCH_VOLT_LIM_H_1V7)
    };

    touch_sensor_config_t sensor_cfg =
        TOUCH_SENSOR_DEFAULT_BASIC_CONFIG(
            TOUCH_SAMPLE_COUNT,
            sample_cfg);

    ESP_RETURN_ON_ERROR(
        touch_sensor_new_controller(
            &sensor_cfg,
            &context->sensor),
        TAG,
        "Could not create touch controller");

    context->channel_cfg =
        (touch_channel_config_t) {
            .abs_active_thresh = {1000},
            .charge_speed = TOUCH_CHARGE_SPEED_7,
            .init_charge_volt =
                TOUCH_INIT_CHARGE_VOLT_DEFAULT,
            .group = TOUCH_CHAN_TRIG_GROUP_BOTH,
        };

    esp_err_t err =
        touch_sensor_new_channel(
            context->sensor,
            TOUCH_WAKE_CHANNEL,
            &context->channel_cfg,
            &context->channel);

    if (err != ESP_OK) {
        touch_sensor_del_controller(
            context->sensor);
        context->sensor = NULL;
        return err;
    }

    touch_chan_info_t channel_info = {0};
    err =
        touch_sensor_get_channel_info(
            context->channel,
            &channel_info);

    if (err != ESP_OK ||
        channel_info.chan_gpio != TOUCH_WAKE_GPIO) {
        ESP_LOGE(
            TAG,
            "Touch channel %d mapped to GPIO%d, expected GPIO%d",
            TOUCH_WAKE_CHANNEL,
            channel_info.chan_gpio,
            TOUCH_WAKE_GPIO);
        touch_sensor_del_channel(
            context->channel);
        touch_sensor_del_controller(
            context->sensor);
        memset(context, 0, sizeof(*context));
        return err != ESP_OK ?
            err : ESP_ERR_INVALID_STATE;
    }

    touch_sensor_filter_config_t filter_cfg =
        TOUCH_SENSOR_DEFAULT_FILTER_CONFIG();

    err =
        touch_sensor_config_filter(
            context->sensor,
            &filter_cfg);

    if (err != ESP_OK) {
        touch_sensor_del_channel(
            context->channel);
        touch_sensor_del_controller(
            context->sensor);
        memset(context, 0, sizeof(*context));
    }

    return err;
}

static void touch_context_destroy(
    touch_context_t *context)
{
    if (context == NULL) {
        return;
    }

    if (context->scanning) {
        touch_sensor_stop_continuous_scanning(
            context->sensor);
        context->scanning = false;
    }

    if (context->enabled) {
        touch_sensor_disable(
            context->sensor);
        context->enabled = false;
    }

    if (context->channel != NULL) {
        touch_sensor_del_channel(
            context->channel);
    }

    if (context->sensor != NULL) {
        touch_sensor_del_controller(
            context->sensor);
    }

    memset(context, 0, sizeof(*context));
}

static esp_err_t touch_context_enable(
    touch_context_t *context,
    bool continuous)
{
    esp_err_t err =
        touch_sensor_enable(
            context->sensor);

    if (err != ESP_OK) {
        return err;
    }

    context->enabled = true;

    for (int i = 0;
         i < TOUCH_INITIAL_SCANS;
         ++i) {
        err =
            touch_sensor_trigger_oneshot_scanning(
                context->sensor,
                2000);

        if (err != ESP_OK) {
            return err;
        }
    }

    if (!continuous) {
        return ESP_OK;
    }

    err =
        touch_sensor_start_continuous_scanning(
            context->sensor);

    if (err == ESP_OK) {
        context->scanning = true;
    }

    return err;
}

static esp_err_t read_touch_value(
    const touch_context_t *context,
    uint32_t *value)
{
    uint32_t values[TOUCH_SAMPLE_COUNT] = {0};

    const esp_err_t err =
        touch_channel_read_data(
            context->channel,
            TOUCH_CHAN_DATA_TYPE_SMOOTH,
            values);

    if (err == ESP_OK && value != NULL) {
        *value = values[0];
    }

    return err;
}

static uint32_t trimmed_mean(
    uint32_t *values,
    size_t count,
    uint32_t *minimum,
    uint32_t *maximum)
{
    for (size_t i = 1; i < count; ++i) {
        const uint32_t value = values[i];
        size_t j = i;

        while (j > 0 && values[j - 1] > value) {
            values[j] = values[j - 1];
            --j;
        }

        values[j] = value;
    }

    if (minimum != NULL) {
        *minimum = values[0];
    }

    if (maximum != NULL) {
        *maximum = values[count - 1];
    }

    const size_t trim = count >= 8 ? count / 8 : 0;
    uint64_t total = 0;

    for (size_t i = trim;
         i < count - trim;
         ++i) {
        total += values[i];
    }

    return (uint32_t)(
        total / (count - trim * 2));
}

static esp_err_t report_progress(
    touch_calibration_progress_cb_t progress,
    touch_calibration_stage_t stage,
    uint8_t completed,
    uint8_t total,
    void *context)
{
    return progress != NULL ?
        progress(
            stage,
            completed,
            total,
            context) :
        ESP_OK;
}

static esp_err_t wait_for_state(
    const touch_context_t *context,
    uint32_t threshold,
    bool touched,
    uint32_t timeout_ms)
{
    const TickType_t deadline =
        xTaskGetTickCount() +
        pdMS_TO_TICKS(timeout_ms);
    unsigned consecutive = 0;

    while ((int32_t)(deadline -
                     xTaskGetTickCount()) > 0) {
        uint32_t value = 0;
        ESP_RETURN_ON_ERROR(
            read_touch_value(
                context,
                &value),
            TAG,
            "Could not sample touch sensor");

        const bool matches =
            touched ?
                value <= threshold :
                value >= threshold;

        if (matches) {
            if (++consecutive >= 3) {
                return ESP_OK;
            }
        } else {
            consecutive = 0;
        }

        vTaskDelay(
            pdMS_TO_TICKS(
                TOUCH_CALIBRATION_SAMPLE_MS));
    }

    return ESP_ERR_TIMEOUT;
}

esp_err_t touch_wake_prepare(
    uint8_t threshold_percent)
{
#if SOC_TOUCH_SENSOR_VERSION != 1
#error "KegScaleESPDisplay touch wake currently targets classic ESP32 touch hardware v1"
#endif

    if (threshold_percent <
            TOUCH_WAKE_THRESHOLD_MIN_PERCENT ||
        threshold_percent >
            TOUCH_WAKE_THRESHOLD_MAX_PERCENT) {
        threshold_percent =
            CONFIG_KEG_DISPLAY_TOUCH_THRESHOLD_PERCENT;
    }

    touch_context_t context;
    ESP_RETURN_ON_ERROR(
        touch_context_create(
            &context),
        TAG,
        "Could not initialize touch hardware");

    /*
     * Establish the untouched benchmark immediately before deep sleep.
     * By this point BLE/display work has completed, so a finger that caused
     * the previous wake should normally have been removed.
     */
    ESP_RETURN_ON_ERROR(
        touch_context_enable(
            &context,
            false),
        TAG,
        "Could not enable touch controller");

    ESP_RETURN_ON_ERROR(
        touch_sensor_disable(
            context.sensor),
        TAG,
        "Could not pause touch controller");
    context.enabled = false;

    uint32_t benchmark[TOUCH_SAMPLE_COUNT] = {0};

#if SOC_TOUCH_SUPPORT_BENCHMARK
    ESP_RETURN_ON_ERROR(
        touch_channel_read_data(
            context.channel,
            TOUCH_CHAN_DATA_TYPE_BENCHMARK,
            benchmark),
        TAG,
        "Could not read touch benchmark");
#else
    ESP_RETURN_ON_ERROR(
        touch_channel_read_data(
            context.channel,
            TOUCH_CHAN_DATA_TYPE_SMOOTH,
            benchmark),
        TAG,
        "Could not read touch smooth value");
#endif

    if (benchmark[0] == 0) {
        ESP_LOGE(
            TAG,
            "Touch benchmark is zero; refusing to enter touch wake mode");
        return ESP_ERR_INVALID_STATE;
    }

    const uint32_t threshold =
        (uint32_t)(
            ((uint64_t)benchmark[0] *
             (100U -
              threshold_percent)) /
            100U);

    context.channel_cfg.abs_active_thresh[0] =
        threshold;

    ESP_RETURN_ON_ERROR(
        touch_sensor_reconfig_channel(
            context.channel,
            &context.channel_cfg),
        TAG,
        "Could not configure touch threshold");

    /*
     * Register hardware touch as a deep-sleep wake source. Timer wake is
     * configured independently by the application and can remain enabled.
     */
    touch_sleep_config_t sleep_cfg =
        TOUCH_SENSOR_DEFAULT_DSLP_CONFIG();

    ESP_RETURN_ON_ERROR(
        touch_sensor_config_sleep_wakeup(
            context.sensor,
            &sleep_cfg),
        TAG,
        "Could not enable touch sleep wake");

    ESP_RETURN_ON_ERROR(
        touch_sensor_enable(
            context.sensor),
        TAG,
        "Could not enable touch controller for sleep");
    context.enabled = true;

    ESP_RETURN_ON_ERROR(
        touch_sensor_start_continuous_scanning(
            context.sensor),
        TAG,
        "Could not start touch scanning");
    context.scanning = true;

    ESP_LOGI(
        TAG,
        "Touch wake armed: GPIO%d / channel %d benchmark=%" PRIu32
        " threshold=%" PRIu32 " sensitivity=%d%%",
        TOUCH_WAKE_GPIO,
        TOUCH_WAKE_CHANNEL,
        benchmark[0],
        threshold,
        threshold_percent);

    return ESP_OK;
}

esp_err_t touch_wake_calibrate(
    touch_calibration_progress_cb_t progress,
    void *progress_context,
    touch_calibration_result_t *result)
{
    if (result == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(result, 0, sizeof(*result));
    result->failure =
        TOUCH_CALIBRATION_FAILURE_HARDWARE;

    ESP_RETURN_ON_ERROR(
        report_progress(
            progress,
            TOUCH_CALIBRATION_STAGE_BASELINE,
            0,
            TOUCH_CALIBRATION_VERIFY_TAPS,
            progress_context),
        TAG,
        "Could not show untouched calibration step");

    touch_context_t context;
    esp_err_t err =
        touch_context_create(
            &context);

    if (err != ESP_OK) {
        return err;
    }

    err =
        touch_context_enable(
            &context,
            true);

    if (err != ESP_OK) {
        touch_context_destroy(&context);
        return err;
    }

    vTaskDelay(
        pdMS_TO_TICKS(
            TOUCH_CALIBRATION_SETTLE_MS));

    uint32_t baseline_samples[
        TOUCH_CALIBRATION_BASELINE_SAMPLES];

    for (size_t i = 0;
         i < TOUCH_CALIBRATION_BASELINE_SAMPLES;
         ++i) {
        err =
            read_touch_value(
                &context,
                &baseline_samples[i]);

        if (err != ESP_OK) {
            touch_context_destroy(&context);
            return err;
        }

        vTaskDelay(
            pdMS_TO_TICKS(
                TOUCH_CALIBRATION_SAMPLE_MS));
    }

    uint32_t baseline_min = 0;
    uint32_t baseline_max = 0;
    result->untouched_value =
        trimmed_mean(
            baseline_samples,
            TOUCH_CALIBRATION_BASELINE_SAMPLES,
            &baseline_min,
            &baseline_max);
    result->noise_span =
        baseline_max - baseline_min;

    ESP_LOGI(
        TAG,
        "Touch calibration baseline=%" PRIu32
        " range=%" PRIu32 "-%" PRIu32
        " noise=%" PRIu32,
        result->untouched_value,
        baseline_min,
        baseline_max,
        result->noise_span);

    uint32_t detection_drop =
        result->untouched_value / 200U;
    const uint32_t noise_guard =
        result->noise_span * 5U;

    if (detection_drop < noise_guard) {
        detection_drop = noise_guard;
    }

    if (detection_drop < 20U) {
        detection_drop = 20U;
    }

    if (detection_drop >=
        result->untouched_value) {
        result->failure =
            TOUCH_CALIBRATION_FAILURE_NOISY;
        touch_context_destroy(&context);
        return ESP_ERR_INVALID_STATE;
    }

    err =
        report_progress(
            progress,
            TOUCH_CALIBRATION_STAGE_TOUCH_AND_HOLD,
            0,
            TOUCH_CALIBRATION_VERIFY_TAPS,
            progress_context);

    if (err == ESP_OK) {
        err =
            wait_for_state(
                &context,
                result->untouched_value -
                    detection_drop,
                true,
                TOUCH_CALIBRATION_ACTION_TIMEOUT_MS);
    }

    if (err != ESP_OK) {
        result->failure =
            err == ESP_ERR_TIMEOUT ?
                TOUCH_CALIBRATION_FAILURE_TIMEOUT :
                TOUCH_CALIBRATION_FAILURE_HARDWARE;
        touch_context_destroy(&context);
        return err;
    }

    vTaskDelay(pdMS_TO_TICKS(250));

    uint32_t touched_samples[
        TOUCH_CALIBRATION_TOUCHED_SAMPLES];

    for (size_t i = 0;
         i < TOUCH_CALIBRATION_TOUCHED_SAMPLES;
         ++i) {
        err =
            read_touch_value(
                &context,
                &touched_samples[i]);

        if (err != ESP_OK) {
            result->failure =
                TOUCH_CALIBRATION_FAILURE_HARDWARE;
            touch_context_destroy(&context);
            return err;
        }

        vTaskDelay(
            pdMS_TO_TICKS(
                TOUCH_CALIBRATION_SAMPLE_MS));
    }

    result->touched_value =
        trimmed_mean(
            touched_samples,
            TOUCH_CALIBRATION_TOUCHED_SAMPLES,
            NULL,
            NULL);

    if (result->touched_value >=
        result->untouched_value) {
        result->failure =
            TOUCH_CALIBRATION_FAILURE_WEAK_SIGNAL;
        touch_context_destroy(&context);
        return ESP_ERR_INVALID_RESPONSE;
    }

    const uint32_t signal_drop =
        result->untouched_value -
        result->touched_value;
    uint32_t minimum_signal =
        result->untouched_value / 100U;

    if (minimum_signal < noise_guard) {
        minimum_signal = noise_guard;
    }

    if (signal_drop < minimum_signal) {
        result->failure =
            TOUCH_CALIBRATION_FAILURE_WEAK_SIGNAL;
        touch_context_destroy(&context);
        return ESP_ERR_INVALID_RESPONSE;
    }

    const uint32_t target_drop =
        (signal_drop * 2U) / 5U;
    uint32_t threshold_percent =
        (uint32_t)(
            ((uint64_t)target_drop * 100U +
             result->untouched_value - 1U) /
            result->untouched_value);

    if (threshold_percent <
        TOUCH_WAKE_THRESHOLD_MIN_PERCENT) {
        threshold_percent =
            TOUCH_WAKE_THRESHOLD_MIN_PERCENT;
    } else if (threshold_percent >
               TOUCH_WAKE_THRESHOLD_MAX_PERCENT) {
        threshold_percent =
            TOUCH_WAKE_THRESHOLD_MAX_PERCENT;
    }

    result->threshold_percent =
        (uint8_t)threshold_percent;

    const uint32_t wake_threshold =
        (uint32_t)(
            ((uint64_t)result->untouched_value *
             (100U - threshold_percent)) /
            100U);

    ESP_LOGI(
        TAG,
        "Touch calibration touched=%" PRIu32
        " signal=%" PRIu32
        " threshold=%" PRIu32 " (%u%%)",
        result->touched_value,
        signal_drop,
        wake_threshold,
        (unsigned)result->threshold_percent);

    err =
        report_progress(
            progress,
            TOUCH_CALIBRATION_STAGE_RELEASE,
            0,
            TOUCH_CALIBRATION_VERIFY_TAPS,
            progress_context);

    if (err == ESP_OK) {
        err =
            wait_for_state(
                &context,
                result->untouched_value -
                    detection_drop / 2U,
                false,
                TOUCH_CALIBRATION_ACTION_TIMEOUT_MS);
    }

    if (err != ESP_OK) {
        result->failure =
            err == ESP_ERR_TIMEOUT ?
                TOUCH_CALIBRATION_FAILURE_TIMEOUT :
                TOUCH_CALIBRATION_FAILURE_HARDWARE;
        touch_context_destroy(&context);
        return err;
    }

    for (uint8_t tap = 0;
         tap < TOUCH_CALIBRATION_VERIFY_TAPS;
         ++tap) {
        err =
            report_progress(
                progress,
                TOUCH_CALIBRATION_STAGE_VERIFY,
                tap,
                TOUCH_CALIBRATION_VERIFY_TAPS,
                progress_context);

        if (err == ESP_OK) {
            err =
                wait_for_state(
                    &context,
                    wake_threshold,
                    true,
                    TOUCH_CALIBRATION_ACTION_TIMEOUT_MS);
        }

        if (err == ESP_OK) {
            result->verified_touches =
                (uint8_t)(tap + 1U);
            err =
                report_progress(
                    progress,
                    TOUCH_CALIBRATION_STAGE_VERIFY,
                    result->verified_touches,
                    TOUCH_CALIBRATION_VERIFY_TAPS,
                    progress_context);
        }

        if (err == ESP_OK) {
            err =
                wait_for_state(
                    &context,
                    result->untouched_value -
                        detection_drop / 2U,
                    false,
                    TOUCH_CALIBRATION_ACTION_TIMEOUT_MS);
        }

        if (err != ESP_OK) {
            result->failure =
                err == ESP_ERR_TIMEOUT ?
                    TOUCH_CALIBRATION_FAILURE_TIMEOUT :
                    TOUCH_CALIBRATION_FAILURE_HARDWARE;
            touch_context_destroy(&context);
            return err;
        }
    }

    result->failure =
        TOUCH_CALIBRATION_FAILURE_NONE;
    touch_context_destroy(&context);
    return ESP_OK;
}
