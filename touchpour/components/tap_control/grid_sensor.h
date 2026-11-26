#pragma once
#include "grid_model.h"
#include "control.h"
namespace tap {
// All access except grid_sensor_task is protected by state_mutex.
extern GridStore grid_store;
extern GridFrame grid_frame;
extern GridMatch grid_match;
extern bool grid_sensor_present;
extern int grid_stable_frames;
void grid_sensor_task(void *);
void grid_load();
bool grid_save(const GridStore &);
bool grid_ready(uint32_t now,bool half=false);
Settings grid_settings(const Settings &base);
int grid_half_target(int legacy);
// Small Cup is a single-zone (VL53L0X) feature. With the 8x8 sensor present or
// trained pours enabled it is unavailable: use the trained Small-cup profile.
int grid_small_target(int legacy);

} // namespace tap
