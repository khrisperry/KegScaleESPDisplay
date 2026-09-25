#include "ui_text.h"
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace touchscreen_text {
void append(char *dest, size_t size, size_t &used, const char *text) {
  if (!dest || !size || !text)
    return;
  while (*text && used + 1 < size)
    dest[used++] = *text++;
}

void sanitize(const char *source, char *dest, size_t size) {
  if (!dest || !size)
    return;
  dest[0] = 0;
  if (!source)
    return;

  size_t used = 0;
  for (size_t i = 0; source[i] && used + 1 < size;) {
    const unsigned char c0 = (unsigned char)source[i];
    if (c0 < 0x80) {
      dest[used++] = (char)c0;
      ++i;
      continue;
    }

    const unsigned char c1 = (unsigned char)source[i + 1];
    const unsigned char c2 = c1 ? (unsigned char)source[i + 2] : 0;
    const char *replacement = nullptr;
    size_t consumed = 1;

    if (c0 == 0xC2 && c1) {
      consumed = 2;
      switch (c1) {
      case 0xA0: replacement = " "; break;       // non-breaking space
      case 0xB0: replacement = " deg"; break;    // degree
      case 0xB1: replacement = "+/-"; break;     // plus/minus
      case 0xB7: replacement = "-"; break;       // middle dot
      default: break;
      }
    } else if (c0 == 0xC3 && c1) {
      consumed = 2;
      if (c1 >= 0x80 && c1 <= 0x85) replacement = "A";
      else if (c1 == 0x86) replacement = "AE";
      else if (c1 == 0x87) replacement = "C";
      else if (c1 >= 0x88 && c1 <= 0x8B) replacement = "E";
      else if (c1 >= 0x8C && c1 <= 0x8F) replacement = "I";
      else if (c1 == 0x91) replacement = "N";
      else if ((c1 >= 0x92 && c1 <= 0x96) || c1 == 0x98) replacement = "O";
      else if (c1 >= 0x99 && c1 <= 0x9C) replacement = "U";
      else if (c1 == 0x9D) replacement = "Y";
      else if (c1 == 0x9F) replacement = "ss";
      else if (c1 >= 0xA0 && c1 <= 0xA5) replacement = "a";
      else if (c1 == 0xA6) replacement = "ae";
      else if (c1 == 0xA7) replacement = "c";
      else if (c1 >= 0xA8 && c1 <= 0xAB) replacement = "e";
      else if (c1 >= 0xAC && c1 <= 0xAF) replacement = "i";
      else if (c1 == 0xB1) replacement = "n";
      else if ((c1 >= 0xB2 && c1 <= 0xB6) || c1 == 0xB8) replacement = "o";
      else if (c1 >= 0xB9 && c1 <= 0xBC) replacement = "u";
      else if (c1 == 0xBD || c1 == 0xBF) replacement = "y";
    } else if (c0 == 0xC5 && c1) {
      consumed = 2;
      if (c1 == 0x92) replacement = "OE";
      else if (c1 == 0x93) replacement = "oe";
    } else if (c0 == 0xE2 && c1 && c2) {
      consumed = 3;
      if (c1 == 0x80) {
        if (c2 >= 0x90 && c2 <= 0x94) replacement = "-";
        else if (c2 == 0x98 || c2 == 0x99) replacement = "'";
        else if (c2 == 0x9C || c2 == 0x9D) replacement = "\"";
        else if (c2 == 0xA2) replacement = "-";
        else if (c2 == 0xA6) replacement = "...";
      } else if (c1 == 0x86) {
        if (c2 == 0x90) replacement = "<-";
        else if (c2 == 0x92) replacement = "->";
      } else if (c1 == 0x9A && c2 == 0xA0) {
        replacement = "!";
      } else if (c1 == 0x9C && c2 == 0x93) {
        replacement = "OK";
      }
    } else if ((c0 & 0xF8) == 0xF0) {
      consumed = 4;
    } else if ((c0 & 0xF0) == 0xE0) {
      consumed = 3;
    } else if ((c0 & 0xE0) == 0xC0) {
      consumed = 2;
    }

    append(dest, size, used, replacement ? replacement : "?");
    i += consumed;
  }
  dest[used] = 0;
}

void label_set_text(lv_obj_t *object, const char *text) {
  char clean[512];
  sanitize(text, clean, sizeof(clean));
  lv_label_set_text(object, clean);
}

void label_set_text_fmt(lv_obj_t *object, const char *format, ...) {
  char formatted[512];
  va_list args;
  va_start(args, format);
  vsnprintf(formatted, sizeof(formatted), format, args);
  va_end(args);
  label_set_text(object, formatted);
}

void textarea_set_text(lv_obj_t *object, const char *text) {
  char clean[256];
  sanitize(text, clean, sizeof(clean));
  lv_textarea_set_text(object, clean);
}

void dropdown_set_options(lv_obj_t *object, const char *options) {
  const size_t source_len = options ? strlen(options) : 0;
  const size_t clean_size = source_len * 4 + 1;
  char *clean = clean_size > 1 ? (char *)malloc(clean_size) : nullptr;
  if (clean) {
    sanitize(options, clean, clean_size);
    lv_dropdown_set_options(object, clean);
    free(clean);
    return;
  }
  char fallback[256];
  sanitize(options, fallback, sizeof(fallback));
  lv_dropdown_set_options(object, fallback);
}
} // namespace touchscreen_text
