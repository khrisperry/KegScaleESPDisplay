#pragma once
#include <stdint.h>
#include "esp_err.h"
typedef void *touch_sensor_handle_t;
typedef void *touch_channel_handle_t;
typedef int touch_sensor_sample_config_t;
typedef int touch_sensor_config_t;
typedef int touch_sensor_filter_config_t;
typedef int touch_sleep_config_t;
typedef struct { uint32_t abs_active_thresh[1]; int charge_speed, init_charge_volt, group; } touch_channel_config_t;
typedef struct { int chan_gpio; } touch_chan_info_t;
#define TOUCH_SENSOR_V1_DEFAULT_SAMPLE_CONFIG(...) 0
#define TOUCH_SENSOR_DEFAULT_BASIC_CONFIG(n, c) ((void)(c), (n))
#define TOUCH_SENSOR_DEFAULT_FILTER_CONFIG() 0
#define TOUCH_SENSOR_DEFAULT_DSLP_CONFIG() 0
#define TOUCH_CHARGE_SPEED_7 7
#define TOUCH_INIT_CHARGE_VOLT_DEFAULT 0
#define TOUCH_CHAN_TRIG_GROUP_BOTH 0
#define TOUCH_CHAN_DATA_TYPE_SMOOTH 1
esp_err_t touch_sensor_new_controller(const touch_sensor_config_t *, touch_sensor_handle_t *);
esp_err_t touch_sensor_new_channel(touch_sensor_handle_t, int, const touch_channel_config_t *, touch_channel_handle_t *);
esp_err_t touch_sensor_get_channel_info(touch_channel_handle_t, touch_chan_info_t *);
esp_err_t touch_sensor_config_filter(touch_sensor_handle_t, const touch_sensor_filter_config_t *);
esp_err_t touch_sensor_del_controller(touch_sensor_handle_t);
esp_err_t touch_sensor_del_channel(touch_channel_handle_t);
esp_err_t touch_sensor_enable(touch_sensor_handle_t);
esp_err_t touch_sensor_disable(touch_sensor_handle_t);
esp_err_t touch_sensor_trigger_oneshot_scanning(touch_sensor_handle_t, int);
esp_err_t touch_sensor_start_continuous_scanning(touch_sensor_handle_t);
esp_err_t touch_sensor_stop_continuous_scanning(touch_sensor_handle_t);
esp_err_t touch_channel_read_data(touch_channel_handle_t, int, uint32_t *);
esp_err_t touch_sensor_reconfig_channel(touch_channel_handle_t, const touch_channel_config_t *);
esp_err_t touch_sensor_config_sleep_wakeup(touch_sensor_handle_t, const touch_sleep_config_t *);
