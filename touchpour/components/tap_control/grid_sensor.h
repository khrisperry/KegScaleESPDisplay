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
extern GridTuning grid_tuning;
// Consecutive unrecognized frames and the time of the last recognized frame.
extern int grid_missed_frames;
extern uint32_t grid_last_recognized_at;
void grid_sensor_task(void *);
void grid_load();
bool grid_save(const GridStore &);
bool grid_tuning_save(const GridTuning &);
// During a pour: the latched profile is still recognized, or only a tolerated
// dropout (grid_tuning.dropout_frames, bounded by stale_ms) has occurred.
bool grid_pour_ok(uint32_t now,int selected_profile);
bool grid_ready(uint32_t now,bool half=false);
Settings grid_settings(const Settings &base);
int grid_half_target(int legacy);
// Small Cup is a single-zone (VL53L0X) feature. With the 8x8 sensor present or
// trained pours enabled it is unavailable: use the trained Small-cup profile.
int grid_small_target(int legacy);

} // namespace tap
