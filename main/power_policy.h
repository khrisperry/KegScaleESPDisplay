#pragma once
#include <stdbool.h>
#include <stdint.h>

static inline uint32_t power_next_check_seconds(
    bool frequent, uint8_t failures, uint32_t frequent_seconds, uint32_t safety_seconds)
{
    if (failures == 1) return 300;
    if (failures == 2) return 900;
    if (failures > 2) return safety_seconds;
    return frequent ? frequent_seconds : safety_seconds;
}

static inline bool power_retry_touch(
    uint8_t phase, bool calibrated, bool stable, bool forced)
{
    return phase == 1 && calibrated && !stable && !forced;
}
