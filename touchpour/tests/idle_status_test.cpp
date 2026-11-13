#include "../components/tap_control/idle_status.h"
#include <cassert>
#include <cstring>
#include <cstdio>
using namespace tap;
int main(){
 Settings c;c.servo_calibrated=c.distance_calibrated=true;
 Sample s{600,11,true,1000,1,600,true};
 assert(!strcmp(idle_status(c,s,1000,false),"No cup detected"));
 assert(!strcmp(idle_status(c,s,1000,false,"Emergency stop"),"No cup detected"));
 assert(!strcmp(idle_status(c,s,1000,false,"Servo PWM failure"),"Servo PWM failure"));
 s={75,11,true,1000,2,230,true};
 assert(!strcmp(idle_status(c,s,1000,false),"Cup at fill level"));
 s={230,11,true,1000,3,230,true};
 assert(!strcmp(idle_status(c,s,1000,false),"Hold cup steady"));
 assert(!strcmp(idle_status(c,s,1000,true),"Cup detected - Ready"));
 assert(!strcmp(idle_status(c,s,1500,false),"LiDAR reading unavailable"));
 // A partial cup below the old empty-cup range is eligible for a top-up.
 c.min_mm=237;c.max_mm=267;c.tray_mm=299;
 s={150,11,true,1000,4,150,true};
 assert(!strcmp(idle_status(c,s,1000,false),"Hold cup steady"));
 assert(!strcmp(idle_status(c,s,1000,true),"Cup detected - Ready"));
 s={150,11,true,1000,5,80,true};
 assert(!strcmp(idle_status(c,s,1000,false),"Cup at fill level"));
 s.valid=false;assert(!strcmp(idle_status(c,s,1000,false),"LiDAR reading unavailable"));
 c.distance_calibrated=false;assert(!strcmp(idle_status(c,s,1000,false),"Calibrate fill level on website"));
 c.servo_calibrated=false;assert(!strcmp(idle_status(c,s,1000,false),"Calibrate servo on website"));
 puts("PASS: idle no-cup/full-cup/stability/readiness/freshness/calibration messages independent of prior stop reason");
}
