# Imported AutoTap source

Source: https://github.com/khrisperry/TapLidarMotor

Source tree: `d2a8e4fa3ecdd69badc5bb8eb3bdbead1d197735`

| Original file | Git blob SHA |
|---|---|
| `components/vl53l0x/Arduino.h` | `baa8e1bcd746ba17d81787cef5b36c76d6e85081` |
| `components/vl53l0x/CMakeLists.txt` | `7730397193122e35de45f6b551cdc3562fd75e4c` |
| `components/vl53l0x/LICENSE.txt` | `16b6dd0219e73573b153131899b4c7d7a9a4c12a` |
| `components/vl53l0x/PORT.md` | `e8c0352956e8d8747877816137467db13f56dd81` |
| `components/vl53l0x/VL53L0X.cpp` | `957eac2147d4d633a4ca1a29963205517e709731` |
| `components/vl53l0x/VL53L0X.h` | `ba8212d02cb8b02df7f05bca82f4d9655714c7be` |
| `components/vl53l0x/Wire.cpp` | `8075493c74fc43be6d7de8f19ce670a95c0b5912` |
| `components/vl53l0x/Wire.h` | `f6df411ff7964dc1a490796c1c47a208cd012c16` |
| `main/CMakeLists.txt` | `baa11101771831928cc1f9437299e5bb85fc44ab` |
| `main/Kconfig.projbuild` | `6037e127f459dd3a444e7c9e5a149ed5e8028f2f` |
| `main/app.cpp` | `e68f492e01c88ddbec05b56936573fa3135f91e3` |
| `main/app.h` | `ce8b089945f066874712dc634cee49432924d998` |
| `main/button_gesture.h` | `a72469d471a3a2b6abefc725477406ef8e0c130e` |
| `main/control.h` | `f37ac6cbfc892adf397114a51ac21cb3a2835398` |
| `main/foam_capture.cpp` | `8f92e43f14fdb9475f74d896ec47795e7466d74e` |
| `main/foam_capture.h` | `3fea6f475fc5a4548147ae944439cba957d99401` |
| `main/index.html` | `200cd9e000bc064cf05f98aa23adcbe8af6099d1` |
| `main/network.cpp` | `3c1ae5f562a826ab7a7f09e819d51d9991610f15` |
| `main/pour_history.cpp` | `6ee87460537d3a2cb647ca39cc7f954216ab2c73` |
| `main/pour_history.h` | `0616236043f2c8bbc04d46999d134567e086ada4` |

Adaptations: namespace `tap`, ESP32-S3 GPIOs, anywhere-touch emergency-stop integration, shared Wi-Fi owner/credential store, TouchPour firmware name and website wording. Vendor touch register and panel initialization protocol: Waveshare ESP32-S3-Touch-LCD-2.8-V2-Demo.zip, downloaded 2026-10-08, https://files.waveshare.com/wiki/ESP32-S3-Touch-LCD-2.8/ESP32-S3-Touch-LCD-2.8-V2-Demo.zip.
