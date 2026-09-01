#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define TOUCH_WAKE_GPIO 12
#define TOUCH_WAKE_CHANNEL 5

/**
 * Calibrate GPIO12 / touch channel 5 and configure it as a deep-sleep
 * wake source. The caller may also enable the RTC timer wake source.
 */
esp_err_t touch_wake_prepare(void);

#ifdef __cplusplus
}
#endif
