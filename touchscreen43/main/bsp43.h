#pragma once
#include "esp_err.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_touch.h"
#include "esp_lvgl_port.h"
#define BSP_LCD_H_RES 800
#define BSP_LCD_V_RES 480
typedef struct { int unused; } bsp_display_config_t;
#ifdef __cplusplus
extern "C" {
#endif
esp_err_t bsp_display_new(const bsp_display_config_t *, esp_lcd_panel_handle_t *, esp_lcd_panel_io_handle_t *);
esp_err_t bsp_touch_new(const void *, esp_lcd_touch_handle_t *);
esp_err_t bsp_display_brightness_set(int brightness);
bool bsp_display_lock(uint32_t timeout_ms);
void bsp_display_unlock(void);
#ifdef __cplusplus
}
#endif
