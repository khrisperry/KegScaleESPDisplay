#include "../components/tap_control/control.h"
#include <cassert>
#include <cstdio>
#include <cstring>
using namespace tap;
static Settings config(){Settings c;c.servo_calibrated=c.distance_calibrated=true;return c;}
static void arm(Control &x,const Settings &c,uint32_t start=500,uint32_t seq=1){
  for(uint32_t i=0;i<5;++i){auto t=start-400+i*100;x.update(c,{250,11,true,t,seq+i,250,true},t);}
  assert(x.start(c,start));
}
static Sample missing(uint32_t at=620,uint32_t seq=6){return {65535,0,false,at,seq};}
int main(){
  auto c=config();
  {
    Control x;arm(x,c);x.update(c,missing(),620);
    assert(x.pouring&&x.startup_dropout_pending&&x.startup_dropout_used);
    assert(x.last_valid==500&&x.previous==250); // invalid does not refresh safety baseline
    x.update(c,missing(),630);assert(x.pouring); // same sequence is not a second sample
    x.update(c,{240,11,true,700,7,240,true},700);
    assert(x.pouring&&!x.startup_dropout_pending&&x.last_valid==700);
    x.update(c,missing(710,8),710);assert(!x.pouring); // only one allowance per pour
    arm(x,c,1300,9);x.update(c,missing(1420,14),1420);assert(x.pouring); // rearms per new pour
    x.stop("Emergency stop");assert(!x.pouring); // explicit stop stays immediate
  }
  {
    Control x;arm(x,c);x.update(c,missing(),620);x.update(c,missing(650,7),650);
    assert(!x.pouring&&!strcmp(x.reason,"LiDAR measurement invalid"));
  }
  {
    Control x;arm(x,c);x.update(c,missing(),620);x.update(c,missing(),849);assert(x.pouring);
    x.update(c,missing(),850);assert(!x.pouring);
    assert(!strcmp(x.reason,"LiDAR startup recovery timeout"));
  }
  for(int mm:{80,75,600,350}){
    Control x;arm(x,c);x.update(c,missing(),620);
    x.update(c,{mm,11,true,650,7,250,true},650);assert(!x.pouring); // cutoff/removal/jump not masked
  }
  for(uint32_t at:{750,751,1000}){
    Control x;arm(x,c);x.update(c,missing(at),at);assert(!x.pouring); // outside startup window
  }
  {
    Control x;arm(x,c);auto bad=missing();bad.at=0;x.update(c,bad,620);assert(!x.pouring);
  }
  {
    Control x;arm(x,c);auto bad=missing();bad.mm=75;x.update(c,bad,620);assert(!x.pouring); // not sentinel
  }
  {
    Control x;c.stale_ms=150;arm(x,c);x.update(c,missing(),620);assert(x.pouring);
    x.update(c,missing(),651);assert(!x.pouring);assert(!strcmp(x.reason,"LiDAR measurement stale"));
  }
  {
    Control x;c=config();uint32_t start=UINT32_MAX-100;arm(x,c,start);
    x.update(c,missing(start+120),start+120);assert(x.pouring);
    x.update(c,{240,11,true,start+200,7,240,true},start+200);assert(x.pouring);
    x.update(c,{80,11,true,start+220,8,240,true},start+220);assert(!x.pouring);
  }
  puts("PASS: single startup sentinel recovery, repeated/late/old faults, 350 ms deadline, unchanged stale limit, raw safety, per-pour rearm and clock wrap");
}
