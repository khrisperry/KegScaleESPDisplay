#include "app.h"
#include "bsp/esp32_s3_touch_lcd_4b.h"
#include "lvgl.h"

namespace {
lv_obj_t *pairing_overlay = nullptr;

constexpr uint32_t BG = 0x101c26;
constexpr uint32_t ACCENT = 0x54d6bf;
constexpr uint32_t TEXT = 0xf2f6f8;
constexpr uint32_t MUTED = 0xaec0ca;

lv_obj_t *overlay_label(lv_obj_t *parent, const char *text, int x, int y,
                        int width, const lv_font_t *font) {
  lv_obj_t *label = lv_label_create(parent);
  lv_label_set_text(label, text ? text : "");
  lv_obj_set_pos(label, x, y);
  lv_obj_set_width(label, width);
  lv_obj_set_style_text_font(label, font, 0);
  lv_obj_set_style_text_color(label, lv_color_hex(TEXT), 0);
  lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
  return label;
}

void clear_pairing_overlay_locked() {
  if (pairing_overlay)
    lv_obj_delete(pairing_overlay);
  pairing_overlay = nullptr;
}

void show_pairing_overlay_locked(const char *code) {
  clear_pairing_overlay_locked();

  pairing_overlay = lv_obj_create(lv_screen_active());
  lv_obj_set_pos(pairing_overlay, 0, 0);
  lv_obj_set_size(pairing_overlay, 480, 480);
  lv_obj_remove_flag(pairing_overlay, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(pairing_overlay, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_style_bg_color(pairing_overlay, lv_color_hex(BG), 0);
  lv_obj_set_style_bg_opa(pairing_overlay, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(pairing_overlay, 0, 0);
  lv_obj_set_style_radius(pairing_overlay, 0, 0);
  lv_obj_set_style_pad_all(pairing_overlay, 0, 0);

  overlay_label(pairing_overlay, "CONNECTING TO SCALE", 24, 42, 432,
                &lv_font_montserrat_24);

  lv_obj_t *caption =
      overlay_label(pairing_overlay, "PAIRING CODE", 24, 112, 432,
                    &lv_font_montserrat_18);
  lv_obj_set_style_text_color(caption, lv_color_hex(ACCENT), 0);

  lv_obj_t *code_label =
      overlay_label(pairing_overlay, code && code[0] ? code : "------", 18,
                    154, 444, &lv_font_montserrat_48);
  lv_obj_set_style_text_letter_space(code_label, 8, 0);

  lv_obj_t *instruction = overlay_label(
      pairing_overlay,
      "Enter this code on the scale's Wi-Fi touchscreen setup page.", 38, 250,
      404, &lv_font_montserrat_18);
  lv_obj_set_style_text_line_space(instruction, 6, 0);

  lv_obj_t *status = overlay_label(pairing_overlay, "Waiting for confirmation...",
                                   38, 350, 404,
                                   &lv_font_montserrat_20);
  lv_obj_set_style_text_color(status, lv_color_hex(ACCENT), 0);

  lv_obj_t *note = overlay_label(
      pairing_overlay,
      "Keep this screen open until the scale confirms the connection.", 38,
      405, 404, &lv_font_montserrat_14);
  lv_obj_set_style_text_color(note, lv_color_hex(MUTED), 0);

  lv_obj_move_foreground(pairing_overlay);
}
} // namespace

void touchscreen_ui_pair_code_dispatch(const char *code) {
  // Keep the legacy state machine updated, then cover the normal setup page
  // with an unmistakable full-screen authorization step.
  ui_pair_code(code);

  if (!bsp_display_lock(1000))
    return;
  show_pairing_overlay_locked(code);
  bsp_display_unlock();
}

void touchscreen_ui_paired_dispatch(void) {
  // Let the legacy UI clear its pairing state and return home first, then remove
  // the overlay so the freshly connected dashboard is what the user sees.
  ui_paired();

  if (!bsp_display_lock(1000))
    return;
  clear_pairing_overlay_locked();
  bsp_display_unlock();
}
