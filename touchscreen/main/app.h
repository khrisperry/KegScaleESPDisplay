#pragma once
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include <stddef.h>
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

struct OtaPreferences {
  char channel[16];
  bool auto_install;
};

enum TouchscreenDiscoveryMode {
  TOUCHSCREEN_DISCOVERY_NONE = 0,
  TOUCHSCREEN_DISCOVERY_WEB = 1,
  TOUCHSCREEN_DISCOVERY_SETUP_WIFI = 2,
  TOUCHSCREEN_DISCOVERY_MULTIPLE = 3,
  TOUCHSCREEN_DISCOVERY_NOT_FOUND = 4,
  TOUCHSCREEN_DISCOVERY_ERROR = 5,
};

bool touchscreen_active_scale_paired(void);
void touchscreen_home_discovery_result(int mode, const char *scale_name,
                                       const char *qr_payload,
                                       const char *detail);

extern QueueHandle_t actions;

void ui_start(const Settings &settings);
void ui_state(const State &state);
void ui_message(const char *message);
void ui_pair_code(const char *code);
void ui_result(bool ok, const char *operation, const char *error);
void ui_discovered(const char *host);
void ui_discovered_options(const char *options);
void ui_networks(const char *options);
void ui_paired(void);
void ui_settings_applied(const Settings &settings);
void ui_scale_profiles(const char *primary_host, bool primary_paired,
                       const char *secondary_host, bool secondary_paired,
                       uint8_t active_scale);

void ui_update_checking(void);
void ui_update_status(const char *current_version, const char *latest_version,
                      bool update_available, bool feed_stale);
void ui_update_installing(const char *version);
void ui_update_progress(int percent);
void ui_update_complete(const char *version);
void ui_update_error(const char *message, bool installing);

esp_err_t touchscreen_ota(bool install);
void touchscreen_ota_get_preferences(OtaPreferences *preferences);
esp_err_t touchscreen_ota_save_preferences(const char *channel,
                                           bool auto_install);
esp_err_t touchscreen_ota_request(bool install, bool foreground);
void touchscreen_ota_get_channel(char *channel, size_t size);
bool touchscreen_ota_has_checked(void);
uint64_t touchscreen_ota_last_check_age_seconds(void);
bool touchscreen_ota_auto_install_enabled(void);
void touchscreen_start_ota_scheduler(void);

/* Kept for the legacy main.cpp OTA action. New UI paths call
 * touchscreen_ota_request() directly and do not disturb the scale session. */
void touchscreen_set_ota_install_mode(bool install);
