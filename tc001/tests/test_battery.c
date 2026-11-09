#include <assert.h>
#include <stdio.h>
#include <limits.h>
#include "battery_policy.h"
#include "pixel_ui.h"
int main(void) {
    assert(tc001_battery_calibration_valid(565,656));
    assert(!tc001_battery_calibration_valid(INT_MAX,INT_MIN));
    assert(tc001_battery_calibration_valid(475,665));
    assert(!tc001_battery_calibration_valid(656,565));
    assert(!tc001_battery_calibration_valid(640,650));
    assert(!tc001_battery_calibration_valid(0,656));
    assert(!tc001_battery_calibration_valid(565,1023));
    assert(tc001_battery_percent(565,565,656)==0);
    assert(tc001_battery_percent(656,565,656)==100);
    assert(tc001_battery_percent(610,565,656)==49);
    assert(tc001_battery_percent(633,565,656)==75);
    assert(tc001_battery_percent(500,565,656)==0);
    assert(tc001_battery_percent(800,565,656)==100);
    assert(tc001_battery_percent(0,565,656)==-1);
    assert(tc001_battery_percent(1023,565,656)==-1);
    assert(tc001_battery_percent(600,656,565)==-1);
    for(int raw=101,prior=-1;raw<1000;raw++) {
        int percent=tc001_battery_percent(raw,565,656);
        assert(percent>=prior && percent>=0 && percent<=100);prior=percent;
    }
    pixel_t frame[PIXEL_COUNT];
    pixel_battery(frame,75,true,0);assert(frame[2*32+1].g==214);
    pixel_battery(frame,15,true,0);assert(frame[2*32+1].r==255 && frame[2*32+1].g==56);
    pixel_battery(frame,25,true,0);assert(frame[2*32+1].r==255 && frame[2*32+1].g==160);
    pixel_battery(frame,-10,true,0);assert(frame[224].r==1);
    pixel_battery(frame,150,true,0);assert(frame[255].g==71);
    pixel_battery(frame,75,false,0);
    for(unsigned x=0;x<32;x++)assert(frame[224+x].r==1 && frame[224+x].g==1);
    pixel_state_t offline={0};pixel_render(frame,&offline,PIXEL_BATTERY_PAGE,0);
    pixel_t expected[PIXEL_COUNT];pixel_battery(expected,0,false,0);
    for(unsigned i=0;i<PIXEL_COUNT;i++)assert(frame[i].r==expected[i].r && frame[i].g==expected[i].g && frame[i].b==expected[i].b);
    puts("PASS: battery mapping, revisions/calibration, invalid samples, monotonic percent, colors, clamps, offline availability");
}
