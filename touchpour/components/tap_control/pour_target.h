#pragma once
#include "control.h"

namespace tap {
// A half-full surface is farther from the sensor than a full surface.
// Zero means uncalibrated/disabled. Never infer volume from half the height.
inline bool valid_half_target(const Settings &settings,int target){
  return target==0 || (target>settings.stop_mm && target<settings.max_mm);
}
inline bool target_ready(const Settings &settings,const Control &control,
                         const Sample &sample,uint32_t now,int target){
  return target>=settings.stop_mm && target<settings.max_mm &&
      control.ready(settings,now) && sample.valid &&
      uint32_t(now-sample.at)<=uint32_t(settings.stale_ms) &&
      sample.mm>target && sample.stable_distance()>target;
}
inline bool half_ready(const Settings &settings,const Control &control,
                       const Sample &sample,uint32_t now,int target){
  return target!=0 && valid_half_target(settings,target) &&
      target_ready(settings,control,sample,now,target);
}
// Runtime/diagnostic copy only. The saved full calibration and its binary
// layout stay unchanged, including existing last-five-pour records.
inline Settings for_target(const Settings &settings,int target){
  auto copy=settings;copy.stop_mm=target;return copy;
}
} // namespace tap
