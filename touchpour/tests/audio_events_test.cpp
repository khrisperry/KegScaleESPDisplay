#include "../components/tap_control/audio_events.h"
#include <cassert>
#include <cstdio>
using namespace tap;
int main(){
  SoundEvents e;
  assert(e.update(false,false,false,false,0)==SoundCue::None);
  assert(e.update(true,false,false,false,500)==SoundCue::Cup);
  assert(e.update(true,false,false,false,600)==SoundCue::None);
  assert(e.update(true,true,false,false,700)==SoundCue::Start);
  assert(e.update(false,false,false,true,800)==SoundCue::Complete);
  // Post-stop readiness resets and sensor jitter do not reannounce the cup.
  assert(e.update(false,false,false,false,900)==SoundCue::None);
  assert(e.update(true,false,false,false,1400)==SoundCue::None);
  e.update(false,false,true,false,1500);
  e.update(false,false,true,false,1900);
  assert(e.update(true,false,false,false,2000)==SoundCue::None);
  // Sustained cup removal rearms once.
  e.update(false,false,true,false,2100);
  e.update(false,false,true,false,2700);
  assert(e.update(true,false,false,false,2800)==SoundCue::Cup);
  assert(e.update(true,true,false,false,2900)==SoundCue::Start);
  assert(e.update(false,false,false,false,3000)==SoundCue::Stop);
  assert(e.update(false,false,false,false,3100)==SoundCue::None);
  // Start wins over detection when they occur in the same controller cycle.
  SoundEvents immediate;
  assert(immediate.update(true,true,false,false,1)==SoundCue::Start);
  assert(immediate.update(false,false,false,false,2)==SoundCue::Stop);
  assert(immediate.update(true,false,false,false,500)==SoundCue::None);
  // Millisecond wrap does not break the removal debounce.
  e.update(false,false,true,false,UINT32_MAX-300);
  e.update(false,false,true,false,400);
  assert(e.update(true,false,false,false,500)==SoundCue::Cup);
  puts("PASS: cup sound latch/removal debounce, start/completion/stop, rapid transitions, clock wrap");
}
