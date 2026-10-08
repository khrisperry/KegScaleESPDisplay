#pragma once
#include <stdbool.h>
#include <stdint.h>
#define PIXEL_COUNT 256
#define PIXEL_PAGES 4
typedef struct { uint8_t r,g,b; } pixel_t;
typedef struct {
    unsigned servings;
    float percent, gallons;
    char name[33];
    bool ready, stable, demo;
} pixel_state_t;
// Framebuffer is always logical row-major, independent of physical wiring.
void pixel_message(pixel_t frame[PIXEL_COUNT], const char *text, uint32_t elapsed_ms, uint32_t color);
void pixel_render(pixel_t frame[PIXEL_COUNT], const pixel_state_t *s, unsigned page, uint32_t elapsed_ms);
unsigned pixel_wire_index(unsigned x, unsigned y, unsigned layout);
