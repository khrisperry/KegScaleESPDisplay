#include <assert.h>
#include "../main/power_policy.h"

int main(void)
{
    for (unsigned frequent = 0; frequent < 2; ++frequent) {
        assert(power_next_check_seconds(frequent, 0, 180, 3600) == (frequent ? 180 : 3600));
        assert(power_next_check_seconds(frequent, 1, 180, 3600) == 300);
        assert(power_next_check_seconds(frequent, 2, 180, 3600) == 900);
        for (unsigned failures = 3; failures < 256; ++failures)
            assert(power_next_check_seconds(frequent, failures, 180, 3600) == 3600);
    }
    for (unsigned phase = 0; phase < 3; ++phase)
        for (unsigned calibrated = 0; calibrated < 2; ++calibrated)
            for (unsigned stable = 0; stable < 2; ++stable)
                for (unsigned forced = 0; forced < 2; ++forced)
                    assert(power_retry_touch(phase, calibrated, stable, forced) ==
                           (phase == 1 && calibrated && !stable && !forced));
    return 0;
}
