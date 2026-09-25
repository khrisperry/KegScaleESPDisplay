#pragma once

#include "lvgl.h"
#include <cstddef>

namespace touchscreen_text {
void sanitize(const char *source, char *dest, size_t size);
void label_set_text(lv_obj_t *object, const char *text);
void label_set_text_fmt(lv_obj_t *object, const char *format, ...);
void textarea_set_text(lv_obj_t *object, const char *text);
void dropdown_set_options(lv_obj_t *object, const char *options);
} // namespace touchscreen_text
