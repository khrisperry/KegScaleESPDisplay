#pragma once
#include <cstdint>

namespace tap::pin_lock {
enum class Result { Ok, WrongPin, Wait, InvalidPin, StorageError };
// Initialize after NVS. Configuration is separate from calibration/Wi-Fi.
void init();
bool configured();
bool locked();
bool lock();
Result check(const char *pin);
Result unlock(const char *pin);
Result change(const char *current_pin,const char *new_pin);
Result remove(const char *current_pin);
uint32_t retry_seconds();
} // namespace tap::pin_lock
