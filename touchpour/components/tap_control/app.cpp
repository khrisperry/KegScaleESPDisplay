#include "tap_app.h"
#include "foam_capture.h"
#include "button_gesture.h"
#include "pour_history.h"
#include "VL53L0X.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs.h"
#include "nvs_flash.h"
#include <cstring>

namespace tap {
Settings settings;
Sample sample;
Control control;
Control manual_hold;
SemaphoreHandle_t state_mutex;
QueueHandle_t actions;
std::atomic<bool> stop_requested{false};
std::atomic<bool> setup_mode{false};
bool manual_test_active = false;
bool firmware_updating = false;
bool update_closed = false;
int servo_us = 0;
static_assert(CONFIG_TAP_SDA == 11 && CONFIG_TAP_SCL == 10 &&
              CONFIG_TAP_SERVO == 15 && CONFIG_TAP_BUTTON == 18,
              "TouchPour reserves exposed I2C 11/10, servo 15, button 18");
std::atomic<bool> touch_stop_armed{false};
bool busy() {
  if (!state_mutex) return true;
  xSemaphoreTake(state_mutex, portMAX_DELAY);
  bool value = pouring_active() || manual_test_active || firmware_updating || history_saving;
  xSemaphoreGive(state_mutex);
  return value;
}
static const char *TAG = "tap";
static uint32_t now_ms() { return esp_timer_get_time() / 1000; }
bool save_settings(const Settings &c) {
  nvs_handle_t h;
  if (nvs_open("tap", NVS_READWRITE, &h) != ESP_OK)
    return false;
  esp_err_t e = nvs_set_blob(h, "settings", &c, sizeof(c));
  if (e == ESP_OK)
    e = nvs_commit(h);
  nvs_close(h);
  return e == ESP_OK;
}
static void set_servo(int us) {
  if (us == servo_us)
    return;
  uint32_t duty = (uint32_t)((uint64_t)us * 16384 / 20000);
  esp_err_t e = ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, duty);
  if (e == ESP_OK)
    e = ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
  if (e != ESP_OK) {
    control.stop("Servo PWM failure");
    manual_hold.stop("Servo PWM failure");
    ESP_LOGE(TAG, "PWM error %s", esp_err_to_name(e));
    return;
  }
  servo_us = us;
  ESP_LOGI(TAG, "Servo command: %d us", us);
}
static void sensor_task(void *) {
  VL53L0X lidar;
  bool bus_ok = Wire.begin(CONFIG_TAP_SDA, CONFIG_TAP_SCL);
  lidar.setTimeout(120);
  bool initialized = false;
  uint32_t seq = 0;
  for (;;) {
    Wire.healthy = true;
    int mm = 0;
    if (!initialized) {
      initialized = bus_ok && lidar.init() && Wire.healthy;
      if (initialized) {
        initialized = lidar.setMeasurementTimingBudget(50000);
        lidar.startContinuous(100);
        initialized &= Wire.healthy;
      }
    }
    bool valid = false;
    if (initialized) {
      mm = lidar.readRangeContinuousMillimeters();
      valid = Wire.healthy && !lidar.timeoutOccurred() &&
              lidar.range_status == 11 && mm >= 20 && mm <= 2000;
      if (!Wire.healthy)
        initialized = false;
    }
    Sample s{mm, lidar.range_status, valid, now_ms(), ++seq};
    xSemaphoreTake(state_mutex, portMAX_DELAY);
    sample = s;
    xSemaphoreGive(state_mutex);
    vTaskDelay(pdMS_TO_TICKS(initialized ? 20 : 1000));
  }
}
static void controller_task(void *) {
  bool initial_high = gpio_get_level((gpio_num_t)CONFIG_TAP_BUTTON);
  ButtonGesture button(initial_high, now_ms());
  uint32_t manual_deadline = 0;

  ESP_LOGI(TAG, "Button GPIO%d: active-low, debounce=30 ms, tap starts on release, "
               "hold=2000 ms for manual fill; initial=%s", CONFIG_TAP_BUTTON,
           initial_high ? "released" : "held low (release to arm)");
  for (;;) {
    bool raw_high = gpio_get_level((gpio_num_t)CONFIG_TAP_BUTTON);
    xSemaphoreTake(state_mutex, portMAX_DELAY);
    uint32_t now = now_ms();
    bool was = pouring_active(), held_was = manual_hold.pouring;
    auto event = button.update(raw_high, now, was || manual_deadline || setup_mode || firmware_updating || history_saving);
    const char *button_event = event.pressed ? "PRESSED (release to start; hold for manual fill)"
                             : event.released ? "RELEASED" : nullptr;
    // Release of an active manual hold is acted on immediately, without waiting
    // for the release debounce. A bouncing release closes rather than reopening.
    if (manual_hold.pouring && raw_high) {
      manual_hold.stop("Manual fill: button released");
      control.stop(manual_hold.reason);
      button.cancelUntilRelease();
      button_event = "HOLD RELEASED / CLOSE";
    }
    if (!manual_hold.pouring) control.update(settings, sample, now);
    // The operator controls manual fill. LiDAR readings never authorize or
    // interrupt it; retain only the independent maximum-time limit.
    if (manual_hold.pouring && now - manual_hold.started >= (uint32_t)settings.max_pour_ms)
      manual_hold.stop("Maximum pour time");
    if (held_was && !manual_hold.pouring) {
      control.stop(manual_hold.reason);
      button.cancelUntilRelease();
      if (!raw_high) button_event = "HOLD CANCELLED / CLOSE";
    }
    if (firmware_updating) {
      button.cancelUntilRelease();
      control.stop("Firmware update: tap closed");
      manual_hold.stop(control.reason);
      manual_deadline = 0;
      manual_test_active = false;
      xQueueReset(actions);
      set_servo(settings.closed_us);
      update_closed = servo_us == settings.closed_us;
      touch_stop_armed = false;
      stop_requested = false;
      xSemaphoreGive(state_mutex);
      if (event.pressed)
        ESP_LOGI(TAG, "BUTTON GPIO%d pressed: ignored during firmware update", CONFIG_TAP_BUTTON);
      vTaskDelay(pdMS_TO_TICKS(10));
      continue;
    }
    bool stop = stop_requested.exchange(false) || (event.pressed && (was || manual_deadline));
    if (stop) {
      button.cancelUntilRelease();
      control.stop("Emergency stop");
      manual_hold.stop(control.reason);
      manual_deadline = 0;
      xQueueReset(actions);
      set_servo(settings.closed_us);
      if (event.pressed) button_event = "STOP / CLOSE";
    }
    if (was && !pouring_active()) set_servo(settings.closed_us);
    if (manual_deadline && (int32_t)(now - manual_deadline) >= 0) {
      set_servo(settings.closed_us);
      manual_deadline = 0;
      control.stop("Servo test completed");
    }
    Action a{};
    bool got = xQueueReceive(actions, &a, 0) == pdTRUE;
    if (event.short_press && !was && !stop) { a = {Action::START, 0}; got = true; }
    if (got && !stop) {
      if (a.kind == Action::CLOSE) {
        button.cancelUntilRelease();
        control.stop("Manual close"); manual_hold.stop(control.reason);
        manual_deadline = 0; set_servo(settings.closed_us);
      } else if (a.kind == Action::TEST && !pouring_active() && !history_saving) {
        button.cancelUntilRelease();
        control.reset_ready(); manual_hold.reset_ready();
        set_servo(a.pulse); manual_deadline = now + 2000;
        control.reason = "Servo test: closes after two seconds";
      } else if (a.kind == Action::START && !setup_mode && !manual_deadline && !history_saving &&
                 !pouring_active() && control.start(settings, now)) {
        button.cancelUntilRelease(); manual_hold.reset_ready();
        set_servo(settings.open_us);
      }
      if (event.short_press) button_event = control.pouring ? "SHORT PRESS STARTED" : "SHORT PRESS REJECTED";
    }
    if (event.long_press && !stop && !got) {
      bool accepted = !setup_mode && !manual_deadline && !history_saving && !pouring_active() &&
                      settings.servo_calibrated;
      if (accepted) {
        manual_hold.pouring = true;
        manual_hold.started = now;
        manual_hold.reason = "Manual fill: release button to close (all LiDAR checks bypassed)";
        control.stop(manual_hold.reason);
        set_servo(settings.open_us);
      } else {
        manual_hold.stop("Manual fill rejected: servo calibration required or device busy");
        control.stop(manual_hold.reason);
        button.cancelUntilRelease();
      }
      button_event = manual_hold.pouring ? "LONG HOLD STARTED" : "LONG HOLD REJECTED";
    }
    if (settings.servo_calibrated && !pouring_active() && !manual_deadline)
      set_servo(settings.closed_us);
    manual_test_active = manual_deadline != 0;
    touch_stop_armed = pouring_active() || manual_test_active;
    foam_observe(sample, settings, now_ms(), servo_us, pouring_active(),
                 control.ready(settings, now) && !pouring_active() && !setup_mode && !manual_test_active,
                 manual_test_active, event.pressed || event.short_press || event.long_press,
                 manual_hold.pouring ? manual_hold.count : control.count,
                 manual_hold.pouring ? manual_hold.reason : control.reason, manual_hold.pouring);
    const char *reason = manual_hold.pouring ? manual_hold.reason : control.reason;
    history_observe(sample, settings, now_ms(), servo_us, pouring_active(),
                    manual_hold.pouring, event.pressed || event.short_press || event.long_press, reason);
    bool stopped = was && !pouring_active();
    int mm = sample.mm, pulse = servo_us, cutoff = settings.stop_mm;
    uint32_t age = now - sample.at, sequence = sample.sequence;
    xSemaphoreGive(state_mutex);
    if (stopped)
      ESP_LOGI(TAG, "POUR STOP: mode=%s reason=%s distance=%d mm cutoff=%d mm cutoff_applied=%d lidar_checks_applied=%d age=%lu ms sequence=%lu servo=%d us",
               held_was ? "MANUAL_HOLD" : "NORMAL", reason, mm, cutoff, !held_was, !held_was,
               (unsigned long)age, (unsigned long)sequence, pulse);
    if (button_event)
      ESP_LOGI(TAG, "BUTTON GPIO%d: %s; reason=%s; distance=%d mm; servo=%d us",
               CONFIG_TAP_BUTTON, button_event, reason, mm, pulse);
    vTaskDelay(pdMS_TO_TICKS(10));
  }
}
static void measurement_log_task(void *) {
  TickType_t wake = xTaskGetTickCount();
  for (;;) {
    xSemaphoreTake(state_mutex, portMAX_DELAY);
    Sample s = sample; Settings c = settings;
    bool hold = manual_hold.pouring, pouring = pouring_active();
    bool saving = history_saving;
    int pulse = servo_us, stable = hold ? manual_hold.count : control.count;
    uint32_t now = now_ms();
    const char *reason = hold ? manual_hold.reason : control.reason;
    bool ready = control.ready(c, now) && !pouring && !setup_mode && !manual_test_active && !firmware_updating && !saving;
    xSemaphoreGive(state_mutex);
    ESP_LOGI("tap_measure", "mode=%s pouring=%d distance=%d mm valid=%d status=%d age=%lu ms "
             "cutoff=%d mm cutoff_applied=%d lidar_checks_applied=%d min=%d max=%d tray=%d jump=%d stability_spread=%d stale_limit=%d ms stable=%d/5 "
             "servo=%d us max_pour=%d ms history_saving=%d reason=%s ready=%d",
             hold ? "MANUAL_HOLD" : "NORMAL", pouring, s.mm, s.valid, s.status,
             (unsigned long)(now - s.at), c.stop_mm, !hold, !hold,
             c.min_mm, c.max_mm, c.tray_mm, c.jump_mm, c.stable_mm, c.stale_ms, stable,
             pulse, c.max_pour_ms, saving, reason, ready);
    vTaskDelayUntil(&wake, pdMS_TO_TICKS(500));
  }
}
void start() {
  ESP_ERROR_CHECK(nvs_flash_init());
  foam_init();
  state_mutex = xSemaphoreCreateMutex();
  actions = xQueueCreate(4, sizeof(Action));
  configASSERT(state_mutex && actions);
  nvs_handle_t h;
  if (nvs_open("tap", NVS_READONLY, &h) == ESP_OK) {
    Settings loaded;
    size_t n = sizeof(loaded);
    if (nvs_get_blob(h, "settings", &loaded, &n) == ESP_OK &&
        n == sizeof(loaded) && valid_settings(loaded))
      settings = loaded;
    nvs_close(h);
  }
  ledc_timer_config_t t = {};
  t.speed_mode = LEDC_LOW_SPEED_MODE;
  t.duty_resolution = LEDC_TIMER_14_BIT;
  t.timer_num = LEDC_TIMER_0;
  t.freq_hz = 50;
  t.clk_cfg = LEDC_AUTO_CLK;
  ESP_ERROR_CHECK(ledc_timer_config(&t));
  ledc_channel_config_t c = {};
  c.gpio_num = CONFIG_TAP_SERVO;
  c.speed_mode = LEDC_LOW_SPEED_MODE;
  c.channel = LEDC_CHANNEL_0;
  c.timer_sel = LEDC_TIMER_0;
  ESP_ERROR_CHECK(ledc_channel_config(&c));
  if (settings.servo_calibrated)
    set_servo(settings.closed_us);
  gpio_config_t b = {};
  b.pin_bit_mask = 1ULL << CONFIG_TAP_BUTTON;
  b.mode = GPIO_MODE_INPUT;
  b.pull_up_en = GPIO_PULLUP_ENABLE;
  ESP_ERROR_CHECK(gpio_config(&b));
  history_init();
  configASSERT(xTaskCreate(controller_task, "pour_control", 4096, nullptr, 8,
                           nullptr) == pdPASS);
  configASSERT(xTaskCreate(sensor_task, "lidar", 4096, nullptr, 4, nullptr) ==
               pdPASS);
  configASSERT(xTaskCreate(measurement_log_task, "measure_log", 3072, nullptr, 2, nullptr) == pdPASS);

}


} // namespace tap
