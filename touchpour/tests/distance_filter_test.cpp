#include "../components/tap_control/distance_filter.h"
#include "../components/tap_control/control.h"
#include "../components/tap_control/foam_capture.h"
#include <cassert>
#include <cmath>
#include <cstdio>
using namespace tap;
int main() {
  DistanceFilter f;
  assert(f.update(230,true,0,350)==230);
  f.update(230,true,100,350);
  assert(f.update(800,true,200,350)==230);
  assert(f.update(230,true,300,350)==230);
  assert(f.update(0,false,400,350)==0);
  assert(f.update(300,true,500,350)==300);
  assert(f.update(250,true,1000,350)==250);
  f.reset();f.update(230,true,UINT32_MAX-49,350);
  assert(f.update(230,true,50,350)==230);
  // The displayed value rejects isolated spikes but follows real changes.
  f.reset();f.update(230,true,0,350);
  for(uint32_t i=1;i<=20;++i)f.update(300,true,i*100,350);
  assert(f.update(300,true,2100,350)>299);
  Settings c;c.servo_calibrated=c.distance_calibrated=true;
  c.stable_mm=4;c.max_mm=234;
  Control x;
  for(uint32_t i=1;i<=5;++i)
    x.update(c,{i%2?225:235,11,true,i*100,i,230,true},i*100);
  assert(x.ready(c,500));
  x.update(c,{239,11,true,550,10,230,true},550);
  assert(x.ready(c,550));
  assert(x.start(c,550));
  x.update(c,{75,11,true,600,11,230,true},600);
  assert(!x.pouring); // raw cutoff wins over the delayed filtered value
  Control removed;
  for(uint32_t i=1;i<=5;++i)removed.update(c,{230,11,true,i*100,i,230,true},i*100);
  assert(removed.start(c,500));
  removed.update(c,{600,11,true,600,6,230,true},600);
  assert(!removed.pouring); // raw cup removal wins too
  Control invalid;
  invalid.update(c,{250,11,true,100,1,230,true},100);
  invalid.update(c,{0,0,false,200,2,230,true},200);
  assert(invalid.count==0);
  Control full;
  for(uint32_t i=1;i<=5;++i)full.update(c,{75,11,true,i*100,i,230,true},i*100);
  assert(!full.ready(c,500)); // filtered lag cannot authorize a full cup
  static FoamCapture capture;capture.start(0);
  capture.observe({235,11,true,100,1,230.125f,true},c,100,1000,
                  false,false,false,false,"test");
  assert(capture.rows[0].mm==235&&capture.rows[0].filtered_mm==230.125f&&capture.rows[0].filtered_valid);
  puts("PASS: median/EMA, spike, step response, invalid/gap reset, clock wrap, smoothed readiness, immediate raw cutoff/removal, dual capture");
}
