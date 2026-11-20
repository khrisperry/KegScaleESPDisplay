#include "../components/tap_control/foam_capture.h"
#include <cassert>
#include <iostream>
using namespace tap;
static FoamCapture capture;
int main() {
  Settings c;
  auto read = [&](uint32_t time, uint32_t seq, int mm, bool valid = true,
                  bool pouring = false, bool button = false) {
    capture.observe(Sample{mm, valid ? 11 : -1, valid, time, seq}, c, time + 5,
                    pouring ? c.open_us : c.closed_us, pouring, false, false,
                    button, pouring ? "Pouring" : "Fill target reached");
  };
  capture.start(1000);
  capture.observe(Sample{100, 11, true, 900, 1}, c, 1005, c.closed_us, false, false, false, false, "Idle"); assert(capture.count == 0); // No old sample at test start.
  read(1000, 2, 100); assert(capture.count == 1);
  read(1000, 2, 100); assert(capture.count == 1); // Native sample, not 100 Hz controller loop.
  assert(!(capture.rows[0].flags & FoamCapture::DELTA_VALID));
  assert(capture.mark(FoamCapture::SETTLING));
  read(1100, 3, 102);
  assert(capture.rows[1].delta_mm == 2 && capture.rows[1].interval_ms == 100);
  assert(capture.rows[1].markers == FoamCapture::SETTLING);
  assert(capture.rows[1].flags & FoamCapture::DELTA_VALID);
  // A button between samples must be retained for the next sample.
  read(1100, 3, 102, true, false, true); assert(capture.count == 2);
  read(1200, 4, 104); assert(capture.rows[2].flags & FoamCapture::BUTTON);
  assert(capture.rows[2].markers == 0);
  capture.mark(FoamCapture::CUP_REMOVED);
  read(1300, 5, 600); assert(capture.rows[3].delta_mm == 496);
  assert(capture.rows[3].markers == FoamCapture::CUP_REMOVED);
  read(1400, 6, 65535, false);
  assert(capture.rows[4].mm == 65535 && !(capture.rows[4].flags & FoamCapture::VALID));
  read(1500, 7, 600); assert(!(capture.rows[5].flags & FoamCapture::DELTA_VALID));
  read(1600, 8, 601, true, true);
  assert(capture.rows[6].flags & FoamCapture::POURING);
  capture.stop(1700); assert(!capture.active && capture.elapsed(9000) == 700);
  assert(!capture.mark(FoamCapture::SETTLING)); read(1800, 9, 603); assert(capture.count == 7);
  capture.start(2000); assert(capture.count == 0 && capture.active);
  read(122000, 10, 100); assert(capture.active && capture.count == 1); // Continues beyond old two-minute limit.
  read(302000, 11, 100); assert(!capture.active && capture.count == 1); // Deadline even without sensor reads.
  capture.start(0);
  for (uint32_t i = 0; i < FoamCapture::capacity; ++i) read(i, i + 1, 100);
  assert(capture.count == FoamCapture::capacity && !capture.active);
  capture.start(0xfffffff0u); read(0xfffffff5u, 1, 100); read(5, 2, 105);
  assert(capture.count == 2 && capture.rows[1].elapsed_ms == 21 && capture.rows[1].interval_ms == 16);
  std::cout << "PASS: foam recording deduplication, slow/abrupt changes, invalid readings, markers, button retention, stop/restart, bounds, timestamp wrap\n";
}
