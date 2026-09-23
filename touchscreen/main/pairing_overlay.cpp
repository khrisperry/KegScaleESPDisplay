#include "app.h"
#include "bsp/esp32_s3_touch_lcd_4b.h"
#include "lvgl.h"
#include "esp_timer.h"
#include <cstdio>

void touchscreen_pairing_timeout_cleanup();

namespace {
lv_obj_t *pairing_overlay = nullptr;
lv_timer_t *pairing_timeout_timer = nullptr;
lv_obj_t *cancel_button = nullptr;
lv_obj_t *cancel_label = nullptr;
int64_t pairing_deadline_us = 0;
bool cancel_queued = false;

constexpr uint32_t BG = 0x101c26;
constexpr uint32_t ACCENT = 0x54d6bf;
constexpr uint32_t TEXT = 0xf2f6f8;
constexpr uint32_t MUTED = 0xaec0ca;
// TOUCH_BUTTON_STYLE_V5

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
  cancel_button = cancel_label = nullptr;
  cancel_queued = false;
}

void request_pairing_cancel(bool expired) {
  if (cancel_queued) return;
  Action action{};
  snprintf(action.kind, sizeof(action.kind), "cancel_pairing");
  snprintf(action.body, sizeof(action.body), "{\"expired\":%s}", expired ? "true" : "false");
  if (xQueueSend(actions, &action, 0) == pdTRUE) {
    cancel_queued = true;
    lv_obj_add_state(cancel_button, LV_STATE_DISABLED);
    lv_label_set_text(cancel_label, expired ? "Pairing window ended" : "Canceling pairing...");
  }
}
void cancel_pairing(lv_event_t *) { request_pairing_cancel(false); }

lv_obj_t *overlay_button(lv_obj_t *parent, const char *text, int x, int y,
                         int width, lv_event_cb_t callback) {
  lv_obj_t *button = lv_button_create(parent);
  lv_obj_set_pos(button, x, y);
  lv_obj_set_size(button, width, 48);
  lv_obj_set_style_bg_color(button, lv_color_hex(0x2a3945), 0);
  lv_obj_set_style_bg_opa(button, LV_OPA_COVER, 0);
  lv_obj_set_style_border_color(button, lv_color_hex(0x587181), 0);
  lv_obj_set_style_border_width(button, 1, 0);
  lv_obj_set_style_radius(button, 15, 0);
  lv_obj_set_style_bg_color(button, lv_color_hex(0x36505f), LV_STATE_PRESSED);
  lv_obj_set_style_border_color(button, lv_color_hex(ACCENT), LV_STATE_PRESSED);

  lv_obj_t *label = lv_label_create(button);
  lv_label_set_text(label, text);
  lv_obj_set_style_text_color(label, lv_color_hex(TEXT), 0);
  lv_obj_center(label);
  lv_obj_add_event_cb(button, callback, LV_EVENT_CLICKED, nullptr);
  return button;
}

void pairing_tick(lv_timer_t *) {
  if (!pairing_overlay || cancel_queued) return;
  const int64_t remaining = pairing_deadline_us - esp_timer_get_time();
  if (remaining <= 0) { request_pairing_cancel(true); return; }
  const unsigned seconds = (unsigned)((remaining + 999999) / 1000000);
  char text[64];
  snprintf(text, sizeof(text), "Cancel pairing (%u:%02u)", seconds / 60, seconds % 60);
  lv_label_set_text(cancel_label, text);
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
                                   38, 320, 404,
                                   &lv_font_montserrat_20);
  lv_obj_set_style_text_color(status, lv_color_hex(ACCENT), 0);

  lv_obj_t *note = overlay_label(
      pairing_overlay,
      "Time remaining in the Scale pairing window.", 38,
      430, 404, &lv_font_montserrat_14);
  lv_obj_set_style_text_color(note, lv_color_hex(MUTED), 0);

  cancel_button = overlay_button(pairing_overlay, "Cancel pairing", 60, 368, 360, cancel_pairing);
  cancel_label = lv_obj_get_child(cancel_button, 0);
  pairing_timeout_timer = lv_timer_create(pairing_tick, 250, nullptr);
  pairing_tick(nullptr);
  lv_obj_move_foreground(pairing_overlay);
}
} // namespace

void touchscreen_ui_pair_code_dispatch(const char *code) {
  ui_pair_code(code);

  if (!bsp_display_lock(1000))
    return;
  show_pairing_overlay_locked(code);
  bsp_display_unlock();
}

void touchscreen_ui_paired_dispatch(void) {
  ui_paired();

  if (!bsp_display_lock(1000))
    return;
  clear_pairing_overlay_locked();
  bsp_display_unlock();
}

// Called only while holding the LVGL lock (including home-layout callbacks).
bool touchscreen_pairing_overlay_visible() { return pairing_overlay != nullptr; }

void touchscreen_pairing_window(uint32_t seconds) {
  if (!bsp_display_lock(1000)) return;
  pairing_deadline_us = esp_timer_get_time() + (int64_t)seconds * 1000000;
  bsp_display_unlock();
}

void touchscreen_pairing_ended() {
  if (!bsp_display_lock(1000)) return;
  if (pairing_overlay) {
    touchscreen_pairing_timeout_cleanup();
    clear_pairing_overlay_locked();
  }
  bsp_display_unlock();
}
