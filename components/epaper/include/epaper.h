#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "epaper_fonts.h"

#ifdef __cplusplus
extern "C" {
#endif

#define EPAPER_WIDTH 250
#define EPAPER_HEIGHT 122

esp_err_t epaper_init(void);
void epaper_clear(bool black);
void epaper_set_pixel(int x, int y, bool black);
void epaper_fill_rect(int x, int y, int width, int height, bool black);
void epaper_draw_rect(int x, int y, int width, int height, bool black);
void epaper_draw_text(int x, int y, const char *text, int scale, bool black);
int epaper_text_width(const char *text, int scale);
void epaper_draw_text_font(
    int x,
    int y,
    const char *text,
    const epaper_font_t *font,
    bool black);
int epaper_font_text_width(
    const char *text,
    const epaper_font_t *font);
esp_err_t epaper_refresh(void);
/*
 * Fast differential refresh for a small logical landscape rectangle.
 * The y coordinate and height must be aligned to eight pixels.
 */
esp_err_t epaper_refresh_partial(
    int x,
    int y,
    int width,
    int height);
esp_err_t epaper_sleep(void);

#ifdef __cplusplus
}
#endif
