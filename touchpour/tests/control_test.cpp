#include "../components/tap_control/control.h"
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <string_view>
using namespace tap;
static int tests = 0;
static Settings config() {
  Settings c;
  c.servo_calibrated = c.distance_calibrated = true;
  return c;
}
static void ready(Control &x, const Settings &c) {
  for (int i = 1; i <= 5; i++)
    x.update(c, Sample{250, 11, true, (uint32_t)i * 100, (uint32_t)i}, i * 100);
  assert(x.ready(c, 500));
}
int main() {
  Settings c = config();
  assert(valid_settings(c));
  ++tests;
  {
    Control x;
    Settings u;
    ready(x, c);
    assert(!x.start(u, 500));
    ++tests;
  }
  {
    Control x;
    for (int i = 1; i <= 4; i++)
      x.update(c, {250, 11, true, (uint32_t)i * 100, (uint32_t)i}, i * 100);
    assert(!x.start(c, 400));
    ++tests;
  }
  {
    Control x;
    ready(x, c);
    assert(x.start(c, 500));
    x.update(c, {80, 11, true, 600, 6}, 600);
    assert(!x.pouring);
    assert(x.count == 0);
    ++tests;
  }
  {
    Control x;
    ready(x, c);
    assert(x.start(c, 500));
    x.update(c, {30, 11, true, 600, 6}, 600);
    assert(!x.pouring);
    assert(x.reason == std::string_view("Fill target reached"));
    ++tests;
  }
  {
    Control x;
    ready(x, c);
    x.start(c, 500);
    x.update(c, {250, 0, false, 600, 6}, 600);
    assert(!x.pouring);
    ++tests;
  }
  {
    Control x;
    ready(x, c);
    x.start(c, 500);
    x.update(c, {250, 11, true, 500, 5}, 851);
    assert(!x.pouring);
    ++tests;
  }
  {
    Control x;
    ready(x, c);
    x.start(c, 500);
    x.update(c, {600, 11, true, 600, 6}, 600);
    assert(!x.pouring);
    ++tests;
  }
  {
    Control x;
    ready(x, c);
    x.start(c, 500);
    x.update(c, {350, 11, true, 600, 6}, 600);
    assert(!x.pouring);
    ++tests;
  }
  {
    Control x;
    ready(x, c);
    x.start(c, 500);
    x.update(c, {230, 11, true, 600, 6}, 600);
    assert(x.pouring);
    ++tests;
  }
  {
    Control x;
    ready(x, c);
    x.start(c, 500);
    x.update(c, {240, 11, true, 30500, 6}, 30500);
    assert(!x.pouring);
    assert(x.reason == std::string_view("Maximum pour time"));
    ++tests;
  }
  {
    Control x;
    ready(x, c);
    x.start(c, 500);
    x.stop("Emergency stop");
    assert(!x.start(c, 600));
    ++tests;
  }
  {
    Control x;
    for (int i = 1; i <= 8; i++)
      x.update(c, {i % 2 ? 250 : 280, 11, true, (uint32_t)i * 100, (uint32_t)i},
               i * 100);
    assert(!x.ready(c, 800));
    ++tests;
  }
  {
    Control x;
    for (int i = 1; i <= 8; i++)
      x.update(c, {600, 11, true, (uint32_t)i * 100, (uint32_t)i}, i * 100);
    assert(!x.ready(c, 800));
    ++tests;
  }
  {
    Control x;
    ready(x, c);
    assert(!x.start(c, 851));
    ++tests;
  }
  {
    Settings bad = c;
    bad.stop_mm = bad.min_mm;
    assert(!valid_settings(bad));
    bad = c;
    bad.open_us = 2600;
    assert(!valid_settings(bad));
    bad = c;
    bad.open_us = bad.closed_us;
    assert(!valid_settings(bad));
    ++tests;
  }
  {
    Control x;
    uint32_t base = UINT32_MAX - 499;
    for (uint32_t i = 0; i < 5; i++)
      x.update(c, {250, 11, true, base + i * 100, i + 1}, base + i * 100);
    assert(x.start(c, base + 400));
    x.update(c, {230, 11, true, base + 500, 6}, base + 500);
    assert(x.pouring);
    ++tests;
  }
  {
    Control x;
    ready(x, c);
    x.update(c, {250, 11, true, 1000, 6}, 1000);
    assert(!x.ready(c, 1000));
    assert(x.count == 1);
    ++tests;
  }
  {
    Control x;
    ready(x, c);
    x.update(c, {250, 11, true, 0, 6}, 600);
    assert(!x.ready(c, 600));
    ++tests;
  }
  {
    Control x;
    for (uint32_t i = 1; i <= 5; i++)
      x.update(c, {80, 11, true, i * 100, i}, i * 100);
    assert(!x.start(c, 500));
    ++tests;
  }
  {
    Settings bad = c;
    bad.tray_mm = INT32_MIN;
    assert(!valid_settings(bad));
    bad.tray_mm = INT32_MAX;
    assert(!valid_settings(bad));
    ++tests;
  }
  // Real empty-cup calibration: partial cups lie below min_mm, above cutoff.
  Settings topup=c;topup.min_mm=237;topup.max_mm=267;topup.tray_mm=299;
  for(int distance:{81,100,150,200,237,267}) {
    Control x;
    for(uint32_t i=1;i<=4;++i)x.update(topup,{distance,11,true,i*100,i,float(distance),true},i*100);
    assert(!x.start(topup,400));
    x.update(topup,{distance,11,true,500,5,float(distance),true},500);
    assert(x.ready(topup,500));assert(!x.pouring); // requires an explicit start
    assert(x.start(topup,500));
    // Descending raw readings reach the exact cutoff without a large jump.
    uint32_t sequence=6,now=600;
    for(int mm=distance;mm>80;mm-=10) {
      x.update(topup,{mm,11,true,now,sequence,float(distance),true},now);
      assert(x.pouring);now+=50;++sequence;
    }
    x.update(topup,{80,11,true,now,sequence,float(distance),true},now);
    assert(!x.pouring&&x.reason==std::string_view("Fill target reached"));
    ++tests;
  }
  for(auto sample:{Sample{80,11,true,0,0,150,true}, // raw full / smoothed lag
                   Sample{150,11,true,0,0,80,true}, // smoothed full
                   Sample{268,11,true,0,0,268,true}, // outside cup distance
                   Sample{299,11,true,0,0,150,true}, // tray / filter lag
                   Sample{150,0,false,0,0,150,true}}) {
    Control x;
    for(uint32_t i=1;i<=5;++i){sample.at=i*100;sample.sequence=i;x.update(topup,sample,i*100);}
    assert(!x.start(topup,500));++tests;
  }
  {
    Control x;
    for(uint32_t i=1;i<=5;++i)x.update(topup,{150,11,true,i*100,i,150,true},i*100);
    Control stale=x;assert(!stale.start(topup,851)); // stale top-up is rejected
    assert(x.start(topup,500));
    x.update(topup,{299,11,true,600,6,150,true},600);
    assert(!x.pouring); // removal still closes immediately on the raw sample
    ++tests;
  }
  printf("PASS: %d control scenarios\n", tests);
}
