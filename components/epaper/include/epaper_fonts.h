#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t bitmap_offset;
    uint8_t width;
    uint8_t height;
    int8_t x_offset;
    int8_t y_offset;
    uint8_t x_advance;
} epaper_glyph_t;

typedef struct {
    const uint8_t *bitmap;
    const epaper_glyph_t *glyphs;
    uint8_t first;
    uint8_t last;
    uint8_t line_height;
} epaper_font_t;

extern const epaper_font_t EPAPER_FONT_HERO;
extern const epaper_font_t EPAPER_FONT_BODY_LARGE;
extern const epaper_font_t EPAPER_FONT_BODY_MEDIUM;
extern const epaper_font_t EPAPER_FONT_BODY_SMALL;

#ifdef __cplusplus
}
#endif
