// Original Waveshare ESP32-S3-Touch-LCD-4.3 (not 4B/4.3C).
// Pinout, 16 MHz timings and CH422G/GT911 reset sequence from Waveshare's
// ESP32-S3-Touch-LCD-4.3/examples/ESP-IDF/09_lvgl_v9_demo.
#include "bsp43.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_check.h"
#include "esp_lcd_panel_rgb.h"
#include "esp_lcd_touch_gt911.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
static const char *TAG = "touchscreen43";
static i2c_master_bus_handle_t bus;
static i2c_master_dev_handle_t mode_dev, output_dev;
static uint8_t outputs = 0x2e; // TP_RST high, backlight high, USB_SEL low.

static esp_err_t init_bus(void) {
    if (bus) return ESP_OK;
    i2c_master_bus_config_t cfg = {
        .i2c_port = I2C_NUM_0, .sda_io_num = GPIO_NUM_8,
        .scl_io_num = GPIO_NUM_9, .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7, .flags.enable_internal_pullup = true,
    };
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&cfg, &bus), TAG, "I2C bus");
    i2c_device_config_t dev = {.dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = 0x24, .scl_speed_hz = 400000};
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(bus, &dev, &mode_dev), TAG, "CH422G mode");
    dev.device_address = 0x38;
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(bus, &dev, &output_dev), TAG, "CH422G outputs");
    uint8_t mode = 0x01;
    return i2c_master_transmit(mode_dev, &mode, 1, 1000);
}
esp_err_t bsp_display_new(const bsp_display_config_t *unused,
                         esp_lcd_panel_handle_t *panel, esp_lcd_panel_io_handle_t *io) {
    (void)unused;
    *io = NULL; // ST7262 RGB panel has no serial command channel.
    esp_lcd_rgb_panel_config_t cfg = {
        .clk_src = LCD_CLK_SRC_DEFAULT,
        .timings = {.pclk_hz = 16000000, .h_res = 800, .v_res = 480,
            .hsync_pulse_width = 4, .hsync_back_porch = 8, .hsync_front_porch = 8,
            .vsync_pulse_width = 4, .vsync_back_porch = 8, .vsync_front_porch = 8,
            .flags.pclk_active_neg = true},
        .in_color_format = LCD_COLOR_FMT_RGB565,
        .out_color_format = LCD_COLOR_FMT_RGB565,
        .dma_burst_size = 64, .data_width = 16,
        .num_fbs = CONFIG_BSP_LCD_RGB_BUFFER_NUMS,
        .bounce_buffer_size_px = 800 * CONFIG_BSP_LCD_RGB_BOUNCE_BUFFER_HEIGHT,
        .hsync_gpio_num = 46, .vsync_gpio_num = 3, .de_gpio_num = 5,
        .pclk_gpio_num = 7, .disp_gpio_num = -1,
        .data_gpio_nums = {14,38,18,17,10,39,0,45,48,47,21,1,2,42,41,40},
        .flags.fb_in_psram = true,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_rgb_panel(&cfg, panel), TAG, "RGB panel");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(*panel), TAG, "RGB reset");
    return esp_lcd_panel_init(*panel);
}
esp_err_t bsp_touch_new(const void *unused, esp_lcd_touch_handle_t *touch) {
    (void)unused;
    ESP_RETURN_ON_ERROR(init_bus(), TAG, "I2C init");
    gpio_config_t irq = {.pin_bit_mask = 1ULL << 4, .mode = GPIO_MODE_OUTPUT};
    ESP_RETURN_ON_ERROR(gpio_config(&irq), TAG, "GT911 address select");
    outputs = 0x2c;
    ESP_RETURN_ON_ERROR(i2c_master_transmit(output_dev, &outputs, 1, 1000), TAG, "GT911 reset low");
    vTaskDelay(pdMS_TO_TICKS(100));
    ESP_RETURN_ON_ERROR(gpio_set_level(GPIO_NUM_4, 0), TAG, "GT911 INT low");
    vTaskDelay(pdMS_TO_TICKS(100));
    outputs = 0x2e;
    ESP_RETURN_ON_ERROR(i2c_master_transmit(output_dev, &outputs, 1, 1000), TAG, "GT911 reset high");
    vTaskDelay(pdMS_TO_TICKS(200));
    ESP_RETURN_ON_ERROR(gpio_set_direction(GPIO_NUM_4, GPIO_MODE_INPUT), TAG, "GT911 INT release");
    esp_lcd_panel_io_i2c_config_t io_cfg = ESP_LCD_TOUCH_IO_I2C_GT911_CONFIG();
    io_cfg.scl_speed_hz = 400000;
    esp_lcd_panel_io_handle_t io;
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_i2c(bus, &io_cfg, &io), TAG, "GT911 IO");
    esp_lcd_touch_config_t cfg = {.x_max = 800, .y_max = 480,
        .rst_gpio_num = GPIO_NUM_NC, .int_gpio_num = GPIO_NUM_NC};
    return esp_lcd_touch_new_i2c_gt911(io, &cfg, touch);
}
esp_err_t bsp_display_brightness_set(int brightness) {
    ESP_RETURN_ON_ERROR(init_bus(), TAG, "I2C init");
    // CH422G only supports on/off on this board, not the 4B's PWM dimming.
    if (brightness > 0) outputs |= 4; else outputs &= (uint8_t)~4;
    return i2c_master_transmit(output_dev, &outputs, 1, 1000);
}
bool bsp_display_lock(uint32_t timeout_ms) { return lvgl_port_lock(timeout_ms); }
void bsp_display_unlock(void) { lvgl_port_unlock(); }
