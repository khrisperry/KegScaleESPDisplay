# Sensor driver provenance

Upstream: https://github.com/pololu/vl53l0x-arduino (retrieved 2026-10-07). MIT license in LICENSE.txt.

VL53L0X.cpp and VL53L0X.h are upstream code with one local change: expose the internal device range status, captured before acknowledging the measurement interrupt. Device status 11 is treated as valid, matching ST's VL53L0X internal device-error-to-range-status mapping. This is not the public ST RangeStatus value 0.

Wire.h/Wire.cpp implement the subset used by this driver with ESP-IDF's native master-I2C API. Every transaction has a bounded timeout, and a sticky transport-health flag invalidates the entire measurement after any bus error. Arduino.h supplies integer types and a yielding monotonic millisecond clock only; no Arduino SDK component is linked.

Mapping verified against [ST source](https://github.com/STMicroelectronics/Azure-cloud-samples/blob/main/b-l4s5i-iot01a/iar/stm32l4xx_lib/Components/vl53l0x/vl53l0x_api_core.c). The upstream lightweight driver does not run ST’s software sigma estimator; V1 also applies fresh-reading, stability and jump checks. Hardware optical acceptance is required.
