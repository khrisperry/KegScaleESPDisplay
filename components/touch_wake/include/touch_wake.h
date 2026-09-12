#pragma once

#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define TOUCH_WAKE_GPIO 12
#define TOUCH_WAKE_CHANNEL 5
#define TOUCH_WAKE_THRESHOLD_MIN_PERCENT 1
#define TOUCH_WAKE_THRESHOLD_MAX_PERCENT 50

typedef enum {
    TOUCH_CALIBRATION_STAGE_PREPARE = 0,
    TOUCH_CALIBRATION_STAGE_BASELINE,
    TOUCH_CALIBRATION_STAGE_TOUCH_AND_HOLD,
    TOUCH_CALIBRATION_STAGE_RELEASE,
    TOUCH_CALIBRATION_STAGE_VERIFY,
} touch_calibration_stage_t;

typedef enum {
    TOUCH_CALIBRATION_FAILURE_NONE = 0,
    TOUCH_CALIBRATION_FAILURE_TIMEOUT,
    TOUCH_CALIBRATION_FAILURE_NOISY,
    TOUCH_CALIBRATION_FAILURE_WEAK_SIGNAL,
    TOUCH_CALIBRATION_FAILURE_HARDWARE,
} touch_calibration_failure_t;

typedef struct {
    uint32_t untouched_value;
    uint32_t touched_value;
    uint32_t noise_span;
    uint8_t threshold_percent;
    uint8_t verified_touches;
    touch_calibration_failure_t failure;
} touch_calibration_result_t;

typedef esp_err_t (*touch_calibration_progress_cb_t)(
    touch_calibration_stage_t stage,
    uint8_t completed,
    uint8_t total,
    void *context);

/**
 * Calibrate GPIO12 / touch channel 5 and configure it as a deep-sleep
 * wake source. The caller may also enable the RTC timer wake source.
 */
esp_err_t touch_wake_prepare(
    uint8_t threshold_percent);

/**
 * Run an awake, user-guided calibration. The progress callback is used by the
 * application to render each instruction on the e-paper display.
 */
esp_err_t touch_wake_calibrate(
    touch_calibration_progress_cb_t progress,
    void *context,
    touch_calibration_result_t *result);

#ifdef __cplusplus
}
#endif
