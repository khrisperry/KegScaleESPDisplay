#pragma once
#include "control.h"
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
struct Action {
  enum Kind { START, TEST, CLOSE } kind;
  int pulse;
};
void network_start();
void start();
bool busy();
extern std::atomic<bool> touch_stop_armed;
bool save_settings(const Settings &);


} // namespace tap
