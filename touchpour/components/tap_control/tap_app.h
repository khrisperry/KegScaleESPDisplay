#pragma once
#include "control.h"
#include "pour_target.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include <atomic>

namespace tap {
extern Settings settings;
extern Sample sample;
extern Control control;
extern Control manual_hold;
// Call with state_mutex held.
inline bool pouring_active() { return control.pouring || manual_hold.pouring; }
extern SemaphoreHandle_t state_mutex;
extern QueueHandle_t actions;
extern std::atomic<bool> stop_requested;
extern std::atomic<bool> setup_mode;
extern bool manual_test_active;
extern bool firmware_updating;
extern bool update_closed;
extern int servo_us;
extern int half_stop_mm,active_stop_mm;
struct Action {
  enum Kind { START, TEST, CLOSE, START_HALF, SCREEN_MANUAL } kind;
  int pulse;
  bool from_web = false; // Web control intentionally remains available when the physical tap is locked.
};
void network_start();
void start();
bool busy();
extern std::atomic<bool> touch_stop_armed;
extern std::atomic<bool> screen_hold_down;
extern std::atomic<uint32_t> screen_hold_at;

bool save_settings(const Settings &,int half_target=-1);


} // namespace tap

