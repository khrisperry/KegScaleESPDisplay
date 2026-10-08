#include "board.h"
#include "emergency_touch.h"
#include "tap_control.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/spi_master.h"
#include "driver/ledc.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lvgl_port.h"
#include "esp_timer.h"
#include "esp_log.h"
#include <atomic>
#include <algorithm>
namespace {
i2c_master_dev_handle_t touch;
bool v2;
EmergencyTouch emergency;
esp_err_t read_reg(uint32_t reg, uint8_t *out, size_t size) {
  uint8_t b[4] = {uint8_t(reg >> 24), uint8_t(reg >> 16), uint8_t(reg >> 8), uint8_t(reg)};
  return i2c_master_transmit_receive(touch, b + (v2 ? 0 : 2), v2 ? 4 : 2, out, size, 20);
}
esp_err_t write_reg(uint32_t reg, const uint8_t *data = nullptr, size_t size = 0) {
  uint8_t b[8] = {uint8_t(reg >> 24), uint8_t(reg >> 16), uint8_t(reg >> 8), uint8_t(reg)};
  if (size) std::copy(data, data + size, b + 4);
  return i2c_master_transmit(touch, b + (v2 ? 0 : 2), (v2 ? 4 : 2) + size, 20);
}
void read_touch(lv_indev_t *input, lv_indev_data_t *data) {
  unsigned count = 0;
  int x = 0, y = 0;
  esp_err_t err = ESP_ERR_NOT_FOUND;
  if (touch && v2) {
    uint8_t b[9]{};
    err = read_reg(0xD0070000, b, sizeof(b));
    if (err == ESP_OK) {
      count = b[3] & 15;
      if (!(b[8] & 0xf0)) count = 0;
      x = ((b[7] & 15) << 8) | b[4];
      y = ((b[7] & 0xf0) << 4) | b[5];
      err = write_reg(0xD00002AB);
    }
  } else if (touch) {
    uint8_t n = 0, b[27]{};
    err = read_reg(0xD005, &n, 1);
    if (err == ESP_OK) {
      count = n & 15;
      if (count) err = read_reg(0xD000, b, sizeof(b));
      if (err == ESP_OK) {
        x = (b[1] << 4) | (b[3] >> 4);
        y = (b[2] << 4) | (b[3] & 15);
        uint8_t clear = 0;
        err = write_reg(0xD005, &clear, 1);
      }
    }
  }
  const bool healthy = err == ESP_OK && count <= 5 && (!count || (x < 240 && y < 320));
  // Process STOP before passing the contact to LVGL, across the entire panel.
  if (emergency.update(healthy, count, tap::touch_stop_armed.load()))
    tap::stop_requested = true;
  if (emergency.consumed) lv_indev_reset(input, nullptr);
  data->state = healthy && count == 1 && !emergency.consumed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
  if (data->state == LV_INDEV_STATE_PRESSED) data->point = {x, y};
}
}
void touchpour_brightness(unsigned percent) {
  ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_1, std::min(percent, 100u) * 8191 / 100);
  ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_1);
}
lv_display_t *touchpour_board_start() {
  // Servo owns LEDC timer/channel 0. Backlight must use its own 5 kHz timer.
  ledc_timer_config_t timer{};
  timer.speed_mode = LEDC_LOW_SPEED_MODE; timer.timer_num = LEDC_TIMER_1;
  timer.duty_resolution = LEDC_TIMER_13_BIT; timer.freq_hz = 5000; timer.clk_cfg = LEDC_AUTO_CLK;
  ESP_ERROR_CHECK(ledc_timer_config(&timer));
  ledc_channel_config_t channel{};
  channel.gpio_num = 5; channel.speed_mode = LEDC_LOW_SPEED_MODE;
  channel.channel = LEDC_CHANNEL_1; channel.timer_sel = LEDC_TIMER_1;
  ESP_ERROR_CHECK(ledc_channel_config(&channel));
  spi_bus_config_t bus{};
  bus.sclk_io_num = GPIO_NUM_40; bus.mosi_io_num = GPIO_NUM_45; bus.miso_io_num = GPIO_NUM_NC;
  bus.quadwp_io_num = bus.quadhd_io_num = GPIO_NUM_NC; bus.max_transfer_sz = 240 * 40 * 2;
  ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO));
  esp_lcd_panel_io_spi_config_t io_cfg{};
  io_cfg.dc_gpio_num = GPIO_NUM_41; io_cfg.cs_gpio_num = GPIO_NUM_42; io_cfg.pclk_hz = 40000000;
  io_cfg.lcd_cmd_bits = io_cfg.lcd_param_bits = 8; io_cfg.trans_queue_depth = 10;
  esp_lcd_panel_io_handle_t io;
  ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(SPI2_HOST, &io_cfg, &io));
  esp_lcd_panel_dev_config_t panel_cfg{};
  panel_cfg.reset_gpio_num = GPIO_NUM_39; panel_cfg.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_BGR;
  panel_cfg.bits_per_pixel = 16;
  esp_lcd_panel_handle_t panel;
  ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(io, &panel_cfg, &panel));
  ESP_ERROR_CHECK(esp_lcd_panel_reset(panel));
  ESP_ERROR_CHECK(esp_lcd_panel_init(panel));
  // Waveshare's ST7789T panel requires these power/porch/gamma settings.
  const struct { uint8_t cmd, n; uint8_t b[14]; } init[] = {
    {0xB0,2,{0x00,0xE8}}, {0xB2,5,{0x0c,0x0c,0x00,0x33,0x33}},
    {0xB7,1,{0x75}}, {0xBB,1,{0x1A}}, {0xC0,1,{0x80}},
    {0xC2,2,{0x01,0xff}}, {0xC3,1,{0x13}}, {0xC4,1,{0x20}},
    {0xC6,1,{0x0F}}, {0xD0,2,{0xA4,0xA1}},
    {0xE0,14,{0xD0,0x0D,0x14,0x0D,0x0D,0x09,0x38,0x44,0x4E,0x3A,0x17,0x18,0x2F,0x30}},
    {0xE1,14,{0xD0,0x09,0x0F,0x08,0x07,0x14,0x37,0x44,0x4D,0x38,0x15,0x16,0x2C,0x2E}}
  };
  for (auto &c : init) ESP_ERROR_CHECK(esp_lcd_panel_io_tx_param(io, c.cmd, c.b, c.n));
  ESP_ERROR_CHECK(esp_lcd_panel_mirror(panel, true, false));
  ESP_ERROR_CHECK(esp_lcd_panel_invert_color(panel, true));
  ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel, true));
  lvgl_port_cfg_t port = ESP_LVGL_PORT_INIT_CONFIG();
  port.task_priority = 5;
  ESP_ERROR_CHECK(lvgl_port_init(&port));
  lvgl_port_display_cfg_t cfg{};
  cfg.io_handle = io; cfg.panel_handle = panel; cfg.buffer_size = 240 * 40;
  cfg.double_buffer = true; cfg.hres = 240; cfg.vres = 320;
  cfg.color_format = LV_COLOR_FORMAT_RGB565;
  cfg.flags.buff_dma = true; cfg.flags.swap_bytes = true;
  auto display = lvgl_port_add_disp(&cfg);
  configASSERT(display);
  gpio_config_t reset{};
  reset.pin_bit_mask = 1ULL << 2; reset.mode = GPIO_MODE_OUTPUT;
  ESP_ERROR_CHECK(gpio_config(&reset));
  gpio_set_level(GPIO_NUM_2, 0); vTaskDelay(pdMS_TO_TICKS(100));
  gpio_set_level(GPIO_NUM_2, 1); vTaskDelay(pdMS_TO_TICKS(500));
  i2c_master_bus_config_t i2c{};
  i2c.i2c_port = I2C_NUM_1; i2c.sda_io_num = GPIO_NUM_1; i2c.scl_io_num = GPIO_NUM_3;
  i2c.clk_source = I2C_CLK_SRC_DEFAULT; i2c.glitch_ignore_cnt = 7; i2c.flags.enable_internal_pullup = true;
  i2c_master_bus_handle_t touch_bus;
  ESP_ERROR_CHECK(i2c_new_master_bus(&i2c, &touch_bus));
  uint8_t addr = 0;
  if (i2c_master_probe(touch_bus, 0x58, 30) == ESP_OK) { addr = 0x58; v2 = true; }
  else if (i2c_master_probe(touch_bus, 0x1a, 30) == ESP_OK) addr = 0x1a;
  if (addr) {
    i2c_device_config_t dev{}; dev.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    dev.device_address = addr; dev.scl_speed_hz = 400000;
    ESP_ERROR_CHECK(i2c_master_bus_add_device(touch_bus, &dev, &touch));
    if (!v2) ESP_ERROR_CHECK(write_reg(0xD109));
    ESP_LOGI("touchpour", "Touch controller: %s", v2 ? "CST3530 (V2)" : "CST328 (V1)");
  } else ESP_LOGE("touchpour", "Touch not detected; manual pouring disabled");
  configASSERT(lvgl_port_lock(0));
  auto input = lv_indev_create(); lv_indev_set_type(input, LV_INDEV_TYPE_POINTER);
  lv_indev_set_display(input, display); lv_indev_set_read_cb(input, read_touch);
  lv_timer_set_period(lv_indev_get_read_timer(input), 20);
  lvgl_port_unlock();
  return display;
}
