#include "../components/tap_control/pour_target.h"
#include <cassert>
#include <cstdio>
using namespace tap;
int main(){
 Settings c;c.servo_calibrated=c.distance_calibrated=true;c.min_mm=237;c.max_mm=267;c.tray_mm=299;
 static_assert(sizeof(Settings)==48,"Existing NVS/history settings layout must not change");
 assert(valid_half_target(c,0)&&valid_half_target(c,160));
 for(int target:{-1,20,80,267,65535})assert(!valid_half_target(c,target));
 Control x;Sample sample;
 for(uint32_t i=1;i<=5;++i){sample={200,11,true,i*100,i,200,true};x.update(c,sample,i*100);}
 assert(!half_ready(c,x,sample,500,0));
 assert(half_ready(c,x,sample,500,160));assert(!half_ready(c,x,sample,851,160));
 auto partial=sample;partial.mm=150;partial.filtered_mm=150;
 assert(target_ready(c,x,partial,500,80));assert(!half_ready(c,x,partial,500,160));
 partial.mm=160;partial.filtered_mm=200;assert(!half_ready(c,x,partial,500,160));
 partial.mm=200;partial.filtered_mm=160;assert(!half_ready(c,x,partial,500,160));
 partial.valid=false;assert(!half_ready(c,x,partial,500,160));
 auto selected=for_target(c,160);assert(c.stop_mm==80&&selected.stop_mm==160);
 assert(x.start(selected,500));
 x.update(selected,{175,11,true,600,6,190,true},600);assert(x.pouring);
 x.update(selected,{160,11,true,700,7,180,true},700);assert(!x.pouring); // raw target wins over smoothing
 Control full;
 for(uint32_t i=1;i<=5;++i)full.update(c,{150,11,true,i*100,i,150,true},i*100);
 assert(full.start(c,500));full.update(c,{120,11,true,600,6,140,true},600);assert(full.pouring);
 full.update(c,{80,11,true,700,7,130,true},700);assert(!full.pouring);
 puts("PASS: separate half calibration, immutable full settings, half/full targets, no overfill start, raw cutoff and stale/invalid rejection");
}
