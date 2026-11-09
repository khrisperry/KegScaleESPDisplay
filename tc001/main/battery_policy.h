#pragma once
#include <stdbool.h>
#define TC001_BATTERY_EMPTY_DEFAULT 565
#define TC001_BATTERY_FULL_DEFAULT 656
static inline bool tc001_battery_calibration_valid(int empty,int full) {
    return empty>=100 && empty<=980 && full>=120 && full<=1000 && full-empty>=20;
}
static inline bool tc001_battery_raw_valid(int raw) { return raw>100 && raw<1000; }
static inline int tc001_battery_percent(int raw,int empty,int full) {
    if(!tc001_battery_calibration_valid(empty,full) || !tc001_battery_raw_valid(raw)) return -1;
    if(raw<=empty) return 0;
    if(raw>=full) return 100;
    return ((raw-empty)*100+(full-empty)/2)/(full-empty);
}
