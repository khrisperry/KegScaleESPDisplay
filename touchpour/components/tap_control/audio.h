#pragma once
#include "audio_events.h"
namespace tap {
void audio_start();
// Nonblocking; the controller never waits for audio playback.
void audio_notify(SoundCue cue);
bool audio_enabled();
bool audio_test();
bool audio_set_enabled(bool enabled);
} // namespace tap
