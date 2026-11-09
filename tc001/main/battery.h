#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
typedef struct {
    int raw,percent,empty,full;
    bool valid;
    int64_t sampled_ms;
} tc001_battery_t;
esp_err_t tc001_battery_start(void);
void tc001_battery_poll(int64_t now_ms);
void tc001_battery_get(tc001_battery_t *out);
esp_err_t tc001_battery_calibrate(int empty,int full);
