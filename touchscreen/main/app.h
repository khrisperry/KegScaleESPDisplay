#pragma once
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include <stdint.h>
struct Settings {
  char ssid[33], password[65], host[128];
  uint8_t master[32];
  bool paired;
  uint8_t brightness;
};
struct Action {
  char kind[24];
  char body[1024];
};
struct State {
  bool online, valid, ready, stable, has_tare, calibrated;
  uint32_t revision;
  uint32_t age_seconds;
  float weight, gallons, servings, percent, capacity, empty, density, serving;
  char name[33], firmware[32];
};
extern QueueHandle_t actions;
void ui_start(const Settings &settings);
void ui_state(const State &state);
void ui_message(const char *message);
void ui_pair_code(const char *code);
void ui_result(bool ok, const char *operation, const char *error);
void ui_discovered(const char *host);
void ui_update_progress(int percent);
void ui_settings_applied(const Settings &settings);
esp_err_t touchscreen_ota(void);

void ui_networks(const char *options);

void ui_paired(void);
