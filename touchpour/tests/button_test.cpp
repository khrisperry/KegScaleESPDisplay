#include "../components/tap_control/button_gesture.h"
#include <cassert>
#include <cstdio>
using namespace tap;
int main() {
  ButtonGesture tap(true, 0);
  assert(!tap.update(false, 10, false).pressed);
  assert(tap.update(false, 40, false).pressed);
  assert(!tap.update(false, 100, false).long_press);
  assert(!tap.update(true, 110, false).short_press);
  assert(tap.update(true, 140, false).short_press);
  ButtonGesture hold(true, 0);
  hold.update(false, 10, false); hold.update(false, 40, false);
  assert(!hold.update(false, 2039, false).long_press);
  assert(hold.update(false, 2040, false).long_press);
  assert(!hold.update(false, 3000, false).long_press);
  hold.update(true, 3010, false);
  assert(!hold.update(true, 3040, false).short_press);
  ButtonGesture busy(true, 0);
  busy.update(false, 10, true); assert(busy.update(false, 40, true).pressed);
  assert(!busy.update(false, 3000, false).long_press);
  busy.update(true, 3010, false); assert(!busy.update(true, 3040, false).short_press);
  ButtonGesture boot(false, 0);
  assert(!boot.update(false, 5000, false).long_press);
  boot.update(true, 5010, false); boot.update(true, 5040, false);
  boot.update(false, 5100, false); assert(boot.update(false, 5130, false).pressed);
  boot.cancelUntilRelease(); assert(!boot.update(false, 8000, false).long_press);
  puts("PASS: tap on release, two-second hold, one-shot/release, busy/startup/cancel lockout");
}
