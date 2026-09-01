#include "touch_wake.h"

#include <inttypes.h>

#include "driver/touch_sens.h"
#include "esp_check.h"
#include "esp_log.h"
#include "sdkconfig.h"
#include "soc/soc_caps.h"

static const char *TAG = "touch_wake";

#define TOUCH_SAMPLE_COUNT 1
#define TOUCH_INITIAL_SCANS 3

esp_err_t touch_wake_prepare(void)
{
#if SOC_TOUCH_SENSOR_VERSION != 1
#error "KegScaleESPDisplay touch wake currently targets classic ESP32 touch hardware v1"
#endif

    touch_sensor_handle_t sensor = NULL;
    touch_channel_handle_t channel = NULL;

    touch_sensor_sample_config_t sample_cfg[TOUCH_SAMPLE_COUNT] = {
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
            &sensor),
        TAG,
        "Could not create touch controller");

    touch_channel_config_t channel_cfg = {
        .abs_active_thresh = {1000},
        .charge_speed = TOUCH_CHARGE_SPEED_7,
        .init_charge_volt =
            TOUCH_INIT_CHARGE_VOLT_DEFAULT,
        .group = TOUCH_CHAN_TRIG_GROUP_BOTH,
    };

    ESP_RETURN_ON_ERROR(
        touch_sensor_new_channel(
            sensor,
            TOUCH_WAKE_CHANNEL,
            &channel_cfg,
            &channel),
        TAG,
        "Could not create touch channel");

    touch_chan_info_t channel_info = {0};

    ESP_RETURN_ON_ERROR(
        touch_sensor_get_channel_info(
            channel,
            &channel_info),
        TAG,
        "Could not read touch channel info");

    if (channel_info.chan_gpio != TOUCH_WAKE_GPIO) {
        ESP_LOGE(
            TAG,
            "Touch channel %d mapped to GPIO%d, expected GPIO%d",
            TOUCH_WAKE_CHANNEL,
            channel_info.chan_gpio,
            TOUCH_WAKE_GPIO);
        return ESP_ERR_INVALID_STATE;
    }

    touch_sensor_filter_config_t filter_cfg =
        TOUCH_SENSOR_DEFAULT_FILTER_CONFIG();

    ESP_RETURN_ON_ERROR(
        touch_sensor_config_filter(
            sensor,
            &filter_cfg),
        TAG,
        "Could not configure touch filter");

    /*
     * Establish the untouched benchmark immediately before deep sleep.
     * By this point BLE/display work has completed, so a finger that caused
     * the previous wake should normally have been removed.
     */
    ESP_RETURN_ON_ERROR(
        touch_sensor_enable(sensor),
        TAG,
        "Could not enable touch controller");

    for (int i = 0;
         i < TOUCH_INITIAL_SCANS;
         ++i) {
        ESP_RETURN_ON_ERROR(
            touch_sensor_trigger_oneshot_scanning(
                sensor,
                2000),
            TAG,
            "Initial touch scan failed");
    }

    ESP_RETURN_ON_ERROR(
        touch_sensor_disable(sensor),
        TAG,
        "Could not pause touch controller");

    uint32_t benchmark[TOUCH_SAMPLE_COUNT] = {0};

#if SOC_TOUCH_SUPPORT_BENCHMARK
    ESP_RETURN_ON_ERROR(
        touch_channel_read_data(
            channel,
            TOUCH_CHAN_DATA_TYPE_BENCHMARK,
            benchmark),
        TAG,
        "Could not read touch benchmark");
#else
    ESP_RETURN_ON_ERROR(
        touch_channel_read_data(
            channel,
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
              CONFIG_KEG_DISPLAY_TOUCH_THRESHOLD_PERCENT)) /
            100U);

    channel_cfg.abs_active_thresh[0] =
        threshold;

    ESP_RETURN_ON_ERROR(
        touch_sensor_reconfig_channel(
            channel,
            &channel_cfg),
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
            sensor,
            &sleep_cfg),
        TAG,
        "Could not enable touch sleep wake");

    ESP_RETURN_ON_ERROR(
        touch_sensor_enable(sensor),
        TAG,
        "Could not enable touch controller for sleep");

    ESP_RETURN_ON_ERROR(
        touch_sensor_start_continuous_scanning(
            sensor),
        TAG,
        "Could not start touch scanning");

    ESP_LOGI(
        TAG,
        "Touch wake armed: GPIO%d / channel %d benchmark=%" PRIu32
        " threshold=%" PRIu32 " sensitivity=%d%%",
        TOUCH_WAKE_GPIO,
        TOUCH_WAKE_CHANNEL,
        benchmark[0],
        threshold,
        CONFIG_KEG_DISPLAY_TOUCH_THRESHOLD_PERCENT);

    return ESP_OK;
}
