#pragma once
#include <cstdint>

namespace tap {
enum class SoundCue { None, Cup, Start, Complete, Stop };
struct SoundEvents {
  bool cup_latched=false, pouring=false, absence_pending=false;
  uint32_t absent_since=0;
  SoundCue update(bool ready, bool active, bool absent, bool complete, uint32_t now) {
    // Internal readiness resets after a stop. Only cup removal rearms this
    // sound, so the same cup cannot chirp repeatedly.
    if (absent && !active) {
      if (!absence_pending) { absence_pending=true; absent_since=now; }
      if (uint32_t(now-absent_since)>=600) cup_latched=false;
    } else absence_pending=false;
    bool detected=ready && !cup_latched;
    if (ready) cup_latched=true;
    SoundCue cue=SoundCue::None;
    if (active && !pouring) cue=SoundCue::Start;
    else if (!active && pouring) cue=complete?SoundCue::Complete:SoundCue::Stop;
    else if (detected && !active) cue=SoundCue::Cup;
    pouring=active;
    return cue;
  }
};
} // namespace tap
