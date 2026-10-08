#include "../main/emergency_touch.h"
#include <cassert>
#include <cstdio>
int main() {
  EmergencyTouch t;
  assert(!t.update(true,1,false)); // Idle press belongs to normal UI.
  assert(!t.update(true,1,true)); // Existing start press does not become STOP.
  assert(!t.update(true,0,true));
  assert(t.update(true,1,true)); // Any new single-finger contact stops.
  assert(t.consumed);
  assert(!t.update(true,1,false)); // Holding after STOP cannot retrigger UI.
  assert(t.consumed);
  assert(!t.update(true,2,false)); assert(t.consumed);
  assert(!t.update(true,0,false)); assert(!t.consumed);
  assert(!t.update(true,1,false)); assert(!t.consumed);
  t.update(true,0,true);
  assert(t.update(true,5,true)); // Multi-touch is also an emergency contact.
  assert(t.consumed);
  t.update(true,0,false);
  assert(t.update(false,0,true)); // I2C failure requests a close.
  assert(t.consumed);
  assert(!t.update(true,1,false)); assert(t.consumed);
  t.update(true,0,false); assert(!t.consumed);
  puts("PASS: any-contact STOP, multi-touch STOP, consumed contact, real release rearm, idle start isolation, touch failure");
}
