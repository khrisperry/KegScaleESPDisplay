#include "app.h"
#include "bsp/esp32_s3_touch_lcd_4b.h"
#include "lvgl.h"

namespace {
lv_obj_t *pairing_overlay = nullptr;
lv_timer_t *pairing_timeout_timer = nullptr;

constexpr uint32_t BG = 0x101c26;
constexpr uint32_t ACCENT = 0x54d6bf;
constexpr uint32_t TEXT = 0xf2f6f8;
constexpr uint32_t MUTED = 0xaec0ca;
constexpr uint32_t PAIRING_TIMEOUT_MS = 120000;

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

void clear_pairing_timer_locked() {
  if (pairing_timeout_timer)
    lv_timer_delete(pairing_timeout_timer);
  pairing_timeout_timer = nullptr;
}

void clear_pairing_overlay_locked() {
  clear_pairing_timer_locked();
  if (pairing_overlay)
    lv_obj_delete(pairing_overlay);
  pairing_overlay = nullptr;
}

void back_to_setup(lv_event_t *) { clear_pairing_overlay_locked(); }

lv_obj_t *overlay_button(lv_obj_t *parent, const char *text, int x, int y,
                         int width, lv_event_cb_t callback) {
  lv_obj_t *button = lv_button_create(parent);
  lv_obj_set_pos(button, x, y);
  lv_obj_set_size(button, width, 48);
  lv_obj_set_style_bg_color(button, lv_color_hex(ACCENT), 0);
  lv_obj_set_style_radius(button, 10, 0);
  lv_obj_t *label = lv_label_create(button);
  lv_label_set_text(label, text);
  lv_obj_set_style_text_color(label, lv_color_hex(BG), 0);
  lv_obj_center(label);
  lv_obj_add_event_cb(button, callback, LV_EVENT_CLICKED, nullptr);
  return button;
}

void pairing_timed_out(lv_timer_t *) {
  pairing_timeout_timer = nullptr;
  if (!pairing_overlay)
    return;

  lv_obj_clean(pairing_overlay);
  overlay_label(pairing_overlay, "PAIRING TIMED OUT", 24, 72, 432,
                &lv_font_montserrat_28);

  lv_obj_t *message = overlay_label(
      pairing_overlay,
      "The scale did not confirm this pairing code. Open Add touchscreen on "
      "the scale and try again.",
      38, 155, 404, &lv_font_montserrat_18);
  lv_obj_set_style_text_line_space(message, 6, 0);

  lv_obj_t *note = overlay_label(
      pairing_overlay,
      "A new pairing attempt will generate a new authorization code.", 38, 270,
      404, &lv_font_montserrat_16);
  lv_obj_set_style_text_color(note, lv_color_hex(MUTED), 0);

  overlay_button(pairing_overlay, "Back to Setup", 60, 360, 360,
                 back_to_setup);
  lv_obj_move_foreground(pairing_overlay);
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

  pairing_timeout_timer =
      lv_timer_create(pairing_timed_out, PAIRING_TIMEOUT_MS, nullptr);
  lv_timer_set_repeat_count(pairing_timeout_timer, 1);
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
