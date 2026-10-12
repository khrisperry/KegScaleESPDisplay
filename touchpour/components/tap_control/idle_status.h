#pragma once
#include "control.h"
#include <cstring>

namespace tap {
// Current idle condition, rather than a latched reason from an earlier close.
inline const char *idle_status(const Settings &config,const Sample &sample,
                               uint32_t now,bool ready,const char *reason=nullptr) {
  if(reason && std::strcmp(reason,"Servo PWM failure")==0)return "Servo PWM failure";
  if(!config.servo_calibrated)return "Calibrate servo on website";
  if(!config.distance_calibrated)return "Calibrate fill level on website";
  if(!sample.valid || uint32_t(now-sample.at)>uint32_t(config.stale_ms))
    return "LiDAR reading unavailable";
  if(sample.mm>=config.tray_mm-20 || sample.stable_distance()>config.max_mm)
    return "No cup detected";
  if(sample.mm<=config.stop_mm)return "Cup at fill level";
  if(sample.stable_distance()<config.min_mm)return "Cup outside start range";
  return ready?"Cup detected - Ready":"Hold cup steady";
}
} // namespace tap
