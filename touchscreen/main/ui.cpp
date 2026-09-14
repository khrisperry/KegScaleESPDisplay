#include "app.h"
#include "esp_lcd_touch.h"
#include "bsp/esp32_s3_touch_lcd_4b.h"
#include "bsp/touch.h"
#include "esp_log.h"
#include "cJSON.h"
#include "esp_app_desc.h"
#include "lvgl.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
namespace {
Settings initial{};
State current{};
lv_obj_t *content, *notice, *keyboard, *fields[6], *headline, *detail,
    *connection, *arc, *networks;
int page_id, cal_step;
uint32_t edit_revision;
bool editor_valid;
char pair_code[13];
const uint32_t BG = 0x101c26, CARD = 0x203441, ACCENT = 0x54d6bf,
               TEXT = 0xf2f6f8;
void build(int page);
void message(const char *s) { lv_label_set_text(notice, s); }
bool submit(const char *kind, cJSON *json) {
  Action a{};
  snprintf(a.kind, sizeof(a.kind), "%s", kind);
  char *s = json ? cJSON_PrintUnformatted(json) : nullptr;
  bool valid = !s || strlen(s) < sizeof(a.body);
  snprintf(a.body, sizeof(a.body), "%s", s ? s : "{}");
  free(s);
  if (!valid || xQueueSend(actions, &a, 0) != pdTRUE) {
    message("Busy — please try again");
    return false;
  }
  return true;
}
lv_obj_t *label(lv_obj_t *parent, const char *s, int x, int y, int width,
                const lv_font_t *font = &lv_font_montserrat_18) {
  auto o = lv_label_create(parent);
  lv_label_set_text(o, s);
  lv_obj_set_pos(o, x, y);
  lv_obj_set_width(o, width);
  lv_obj_set_style_text_font(o, font, 0);
  lv_obj_set_style_text_color(o, lv_color_hex(TEXT), 0);
  return o;
}
void button(lv_obj_t *parent, const char *s, int x, int y, int width,
            lv_event_cb_t cb, void *data = nullptr) {
  auto o = lv_button_create(parent);
  lv_obj_set_pos(o, x, y);
  lv_obj_set_size(o, width, 46);
  lv_obj_set_style_bg_color(o, lv_color_hex(ACCENT), 0);
  lv_obj_set_style_radius(o, 10, 0);
  auto t = lv_label_create(o);
  lv_label_set_text(t, s);
  lv_obj_set_style_text_color(t, lv_color_hex(BG), 0);
  lv_obj_center(t);
  lv_obj_add_event_cb(o, cb, LV_EVENT_CLICKED, data);
}
void dismiss_keyboard() {
  if (keyboard) {
    lv_keyboard_set_textarea(keyboard, nullptr);
    lv_obj_delete_async(keyboard);
    keyboard = nullptr;
    lv_obj_set_height(content, 334);
  }
}
void keyboard_event(lv_event_t *) { dismiss_keyboard(); }
void focus(lv_event_t *e) {
  if (!keyboard) {
    keyboard = lv_keyboard_create(lv_screen_active());
    lv_obj_set_size(keyboard, 480, 200);
    lv_obj_align(keyboard, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_add_event_cb(keyboard, keyboard_event, LV_EVENT_READY, nullptr);
    lv_obj_add_event_cb(keyboard, keyboard_event, LV_EVENT_CANCEL, nullptr);
  }
  auto field = (lv_obj_t *)lv_event_get_target(e);
  lv_keyboard_set_textarea(keyboard, field);
  lv_obj_set_height(content, 222);
  lv_obj_scroll_to_view(field, LV_ANIM_OFF);
}
lv_obj_t *field(const char *title, const char *value, int y,
                bool numeric = false, int limit = 32) {
  label(content, title, 8, y, 420, &lv_font_montserrat_16);
  auto o = lv_textarea_create(content);
  lv_obj_set_pos(o, 8, y + 25);
  lv_obj_set_size(o, 424, 46);
  lv_textarea_set_one_line(o, true);
  lv_textarea_set_max_length(o, limit);
  lv_textarea_set_text(o, value);
  if (numeric)
    lv_textarea_set_accepted_chars(o, "0123456789.");
  // Open the editor after a tap, not the press that begins a scroll.
  lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLL_ON_FOCUS);
  lv_obj_add_event_cb(o, focus, LV_EVENT_SHORT_CLICKED, nullptr);
  return o;
}
void command(const char *op) {
  auto o = cJSON_CreateObject();
  cJSON_AddStringToObject(o, "op", op);
  if (submit("command", o))
    message("Waiting for scale confirmation…");
  cJSON_Delete(o);
}
void nav(lv_event_t *e) { build((int)(intptr_t)lv_event_get_user_data(e)); }
void save_keg(lv_event_t *) {
  if (!editor_valid) {
    message("Connection changed. Reload from scale before saving.");
    return;
  }
  dismiss_keyboard();
  auto o = cJSON_CreateObject();
  cJSON_AddStringToObject(o, "op", "save");
  cJSON_AddNumberToObject(o, "revision", edit_revision);
  cJSON_AddStringToObject(o, "name", lv_textarea_get_text(fields[0]));
  const char *keys[] = {"capacity", "empty", "density", "serving"};
  for (int i = 0; i < 4; i++) {
    const char *text = lv_textarea_get_text(fields[i + 1]);
    char *end;
    double v = strtod(text, &end);
    if (end == text || *end || !std::isfinite(v)) {
      message("Complete every numeric field");
      cJSON_Delete(o);
      return;
    }
    cJSON_AddNumberToObject(o, keys[i], v);
  }
  if (submit("command", o))
    message("Saving keg information…");
  cJSON_Delete(o);
}
void replace_keg(lv_event_t *) {
  build(1);
  message("Place the new keg on the calibrated scale, enter its details, then "
          "Save. Do not tare with a keg on the scale.");
}
void calibration(lv_event_t *) {
  if (cal_step == 0)
    command("begin_calibration");
  else if (cal_step == 1)
    command("tare");
  else {
    const char *text = lv_textarea_get_text(fields[0]);
    char *end;
    double weight = strtod(text, &end);
    if (end == text || *end || !std::isfinite(weight) || weight <= 0 ||
        weight > 500) {
      message("Enter a known weight between 0 and 500 lb");
      return;
    }
    auto o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "op", "calibrate");
    cJSON_AddNumberToObject(o, "weight", weight);
    if (submit("command", o))
      message("Calibrating — keep the weight still…");
    cJSON_Delete(o);
    dismiss_keyboard();
  }
}
void cancel_calibration(lv_event_t *) {
  command("cancel_calibration");
  cal_step = 0;
  build(0);
}
void discover(lv_event_t *) {
  dismiss_keyboard();
  submit("discover", nullptr);
  message("Looking for a scale…");
}
void scan(lv_event_t *) {
  dismiss_keyboard();
  submit("scan_wifi", nullptr);
  message("Scanning Wi-Fi…");
}
void select_network(lv_event_t *e) {
  char ssid[33];
  lv_dropdown_get_selected_str((lv_obj_t *)lv_event_get_target(e), ssid,
                               sizeof(ssid));
  lv_textarea_set_text(fields[0], ssid);
}
void save_settings(lv_event_t *) {
  dismiss_keyboard();
  auto o = cJSON_CreateObject();
  cJSON_AddStringToObject(o, "ssid", lv_textarea_get_text(fields[0]));
  cJSON_AddStringToObject(o, "password", lv_textarea_get_text(fields[1]));
  cJSON_AddStringToObject(o, "host", lv_textarea_get_text(fields[2]));
  cJSON_AddNumberToObject(o, "brightness", lv_slider_get_value(fields[3]));
  submit("settings", o);
  cJSON_Delete(o);
}
void brightness(lv_event_t *e) {
  bsp_display_brightness_set(
      lv_slider_get_value((lv_obj_t *)lv_event_get_target(e)));
}
void forget_yes(lv_event_t *) {
  submit("forget", nullptr);
  build(3);
}
void forget(lv_event_t *) {
  dismiss_keyboard();
  lv_obj_clean(content);
  label(content, "Replace the paired scale?", 8, 12, 424,
        &lv_font_montserrat_24);
  label(content,
        "Also remove the old touchscreen pairing on the scale's web page. Then "
        "open Add touchscreen again.",
        8, 64, 424);
  button(content, "Remove pairing", 8, 185, 205, forget_yes);
  button(content, "Cancel", 230, 185, 200, nav, (void *)3);
}
void update_yes(lv_event_t *) {
  submit("ota", nullptr);
  message("Checking touchscreen firmware…");
}
void update(lv_event_t *) {
  dismiss_keyboard();
  lv_obj_clean(content);
  label(content, "Install development firmware?", 8, 12, 424,
        &lv_font_montserrat_24);
  label(content,
        "Dev firmware is for instructed testing. It may be unstable and "
        "require USB recovery. Keep power connected throughout the update.",
        8, 70, 424);
  button(content, "Install update", 8, 220, 205, update_yes);
  button(content, "Cancel", 230, 220, 200, nav, (void *)4);
}
void dashboard() {
  if (!headline)
    return;
  if (page_id == 0) {
    lv_label_set_text(headline,
                      current.name[0] ? current.name : "Set up your keg");
    if (current.ready)
      lv_label_set_text_fmt(detail, "%.0f", (double)floorf(current.servings));
    else
      lv_label_set_text(detail, "--");
    lv_arc_set_value(arc, current.ready ? (int)current.percent : 0);
    char status[80];
    if (current.online)
      snprintf(status, sizeof(status), "Connected");
    else
      snprintf(status, sizeof(status), "Disconnected — last reading %lus ago",
               (unsigned long)current.age_seconds);
    lv_label_set_text_fmt(connection, "%s\n%.2f gal | %.1f oz | %.2f lb | %s",
                          status, (double)current.gallons,
                          (double)current.serving, (double)current.weight,
                          current.stable ? "Stable" : "Settling");

  } else if (page_id == 2) {
    lv_label_set_text_fmt(
        headline, "Scale: %.3f lb   %s", (double)current.weight,
        current.online ? (current.stable ? "Stable" : "Settling")
                       : "Disconnected");
  }
}
void build(int page) {
  dismiss_keyboard();
  page_id = page;
  lv_obj_clean(content);
  headline = detail = connection = arc = networks = nullptr;
  memset(fields, 0, sizeof(fields));
  if (page == 0) {
    headline = label(content, current.name, 8, 0, 424, &lv_font_montserrat_24);
    lv_label_set_long_mode(headline, LV_LABEL_LONG_DOT);
    lv_obj_set_height(headline, 30);
    arc = lv_arc_create(content);
    lv_obj_set_size(arc, 200, 200);
    lv_obj_set_pos(arc, 120, 36);
    lv_arc_set_rotation(arc, 135);
    lv_arc_set_bg_angles(arc, 0, 270);
    lv_arc_set_range(arc, 0, 100);
    lv_obj_remove_style(arc, nullptr, LV_PART_KNOB);
    lv_obj_remove_flag(arc, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_arc_color(arc, lv_color_hex(ACCENT), LV_PART_INDICATOR);
    detail = label(content, "--", 140, 88, 160, &lv_font_montserrat_48);
    auto caption =
        label(content, "Servings left", 130, 149, 180, &lv_font_montserrat_18);
    lv_obj_set_style_text_align(caption, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_align(detail, LV_TEXT_ALIGN_CENTER, 0);
    connection = label(content, "", 8, 240, 424, &lv_font_montserrat_16);
    button(content, "Replace keg", 8, 276, 424, replace_keg);
    dashboard();
  } else if (page == 1) {
    edit_revision = current.revision;
    editor_valid = current.online;
    label(content, "Keg information", 8, 0, 424, &lv_font_montserrat_24);
    fields[0] = field("Beer / beverage name", current.name, 42);
    const char *names[] = {"Keg capacity (gallons)", "Empty keg weight (lb)",
                           "Beverage density (lb / gallon)",
                           "Serving size (oz)"};
    float values[] = {current.capacity, current.empty,
                      current.density > 0 ? current.density : 8.34f,
                      current.serving > 0 ? current.serving : 16};
    for (int i = 0; i < 4; i++) {
      char b[24];
      snprintf(b, sizeof(b), "%.2f", (double)values[i]);
      fields[i + 1] = field(names[i], b, 125 + i * 85, true);
    }
    label(content,
          "Examples: pint 16 oz • can 12 oz\nGrowler 64 oz. Capacity is the "
          "keg's nominal full volume.",
          8, 470, 424, &lv_font_montserrat_16);
    button(content, "Save to scale", 8, 530, 424, save_keg);
    button(content, "Reload from scale", 8, 590, 424, nav, (void *)1);
  } else if (page == 2) {
    headline = label(content, "", 8, 0, 424, &lv_font_montserrat_20);
    dashboard();
    const char *instructions[] = {
        "Scale calibration\n\nUse this when setting up the scale. Have a known "
        "weight ready. Remove the keg before starting.",
        "Step 1: Empty scale\n\nRemove the keg and all objects from the scale. "
        "Leave the scale itself assembled. Keep it still, then save the empty "
        "tare.",
        "Step 2: Known weight\n\nPlace your known weight on the scale. Enter "
        "its weight below and keep it still."};
    label(content, instructions[cal_step], 8, 40, 424);
    if (cal_step == 2)
      fields[0] = field("Known weight (lb)", "30", 145, true);
    button(content,
           cal_step == 0   ? "Start calibration"
           : cal_step == 1 ? "Save empty tare"
                           : "Calibrate with known weight",
           8, cal_step == 2 ? 225 : 210, 424, calibration);
    button(content, "Cancel / release scale", 8, cal_step == 2 ? 275 : 265, 424,
           cancel_calibration);
  } else if (page == 3) {
    label(content, "Connection & display", 8, 0, 424, &lv_font_montserrat_24);
    if (pair_code[0]) {
      char b[140];
      snprintf(b, sizeof(b),
               "Pairing code: %s\nEnter this on the scale's Wi-Fi touchscreen "
               "setup page.",
               pair_code);
      label(content, b, 8, 38, 424, &lv_font_montserrat_20);
    }
    int y = pair_code[0] ? 135 : 45;
    button(content, "Scan Wi-Fi", 8, y, 205, scan);
    button(content, "Find scale", 230, y, 200, discover);
    networks = lv_dropdown_create(content);
    lv_obj_set_pos(networks, 8, y + 56);
    lv_obj_set_size(networks, 424, 45);
    lv_dropdown_set_options(networks, "Select Wi-Fi network");
    lv_obj_add_event_cb(networks, select_network, LV_EVENT_VALUE_CHANGED,
                        nullptr);
    fields[0] = field("Wi-Fi name (SSID)", initial.ssid, y + 115, false, 32);
    fields[1] = field("Wi-Fi password", initial.password, y + 200, false, 64);
    lv_textarea_set_password_mode(fields[1], true);
    fields[2] = field("Scale hostname or IP address", initial.host, y + 285,
                      false, 127);
    label(content,
          "Open Add touchscreen on the scale before connecting for the first "
          "time.",
          8, y + 370, 424, &lv_font_montserrat_16);
    label(content, "Brightness", 8, y + 425, 424);
    fields[3] = lv_slider_create(content);
    lv_obj_set_pos(fields[3], 18, y + 468);
    lv_obj_set_size(fields[3], 404, 15);
    lv_slider_set_range(fields[3], 10, 100);
    lv_slider_set_value(fields[3], initial.brightness, LV_ANIM_OFF);
    lv_obj_add_event_cb(fields[3], brightness, LV_EVENT_VALUE_CHANGED, nullptr);
    button(content, "Save and connect", 8, y + 505, 424, save_settings);
    button(content, "Remove pairing", 8, y + 565, 424, forget);
  } else {
    label(content, "Firmware & diagnostics", 8, 0, 424, &lv_font_montserrat_24);
    char b[300];
    snprintf(b, sizeof(b),
             "Touchscreen: %s\nScale: %s\nWi-Fi protocol: 1\nConnection: "
             "%s\n\nHardware: Waveshare 4B\nThis update is for the "
             "touchscreen. Update the scale from its web page.",
             esp_app_get_description()->version, current.firmware,
             current.online ? "Connected" : "Disconnected");
    label(content, b, 8, 45, 424);
    button(content, "Check / install dev update", 8, 270, 424, update);
  }
}
} // namespace
void ui_start(const Settings &s) {
  initial = s;
  // Use driver-owned frames and wait for bounce-frame completion before reuse.
  static_assert(CONFIG_BSP_LCD_RGB_BUFFER_NUMS >= 2,
                "Set BSP_LCD_RGB_BUFFER_NUMS=2 in menuconfig (Board Support Package)");
  lvgl_port_cfg_t port = ESP_LVGL_PORT_INIT_CONFIG();
  ESP_ERROR_CHECK(lvgl_port_init(&port));
  bsp_display_config_t panel_config = {};
  esp_lcd_panel_handle_t panel = nullptr;
  esp_lcd_panel_io_handle_t io = nullptr;
  ESP_ERROR_CHECK(bsp_display_new(&panel_config, &panel, &io));
  lvgl_port_display_cfg_t cfg = {};
  cfg.io_handle = io;
  cfg.panel_handle = panel;
  cfg.buffer_size = BSP_LCD_H_RES * BSP_LCD_V_RES;
  cfg.double_buffer = true;
  cfg.hres = BSP_LCD_H_RES;
  cfg.vres = BSP_LCD_V_RES;
  cfg.color_format = LV_COLOR_FORMAT_RGB565;
  cfg.flags.full_refresh = true;
  lvgl_port_display_rgb_cfg_t rgb = {};
  rgb.flags.bb_mode = CONFIG_BSP_LCD_RGB_BOUNCE_BUFFER_HEIGHT > 0;
  rgb.flags.avoid_tearing = true;
  auto display = lvgl_port_add_disp_rgb(&cfg, &rgb);
  configASSERT(display);
  esp_lcd_touch_handle_t touch = nullptr;
  ESP_ERROR_CHECK(bsp_touch_new(nullptr, &touch));
  lvgl_port_touch_cfg_t input = {};
  input.disp = display;
  input.handle = touch;
  configASSERT(lvgl_port_add_touch(&input));
  ESP_LOGI("display", "RGB synchronized double framebuffer, bounce height %d",
           CONFIG_BSP_LCD_RGB_BOUNCE_BUFFER_HEIGHT);
  configASSERT(display);
  ESP_ERROR_CHECK(
      bsp_display_brightness_set(s.brightness >= 10 ? s.brightness : 85));
  configASSERT(bsp_display_lock(0));
  auto root = lv_screen_active();
  lv_obj_remove_flag(root, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_style_bg_color(root, lv_color_hex(BG), 0);
  lv_obj_set_style_text_font(root, &lv_font_montserrat_16, 0);
  label(root, "KEG SCALE", 20, 12, 440, &lv_font_montserrat_24);
  content = lv_obj_create(root);
  lv_obj_set_pos(content, 12, 50);
  lv_obj_set_size(content, 456, 334);
  lv_obj_set_scroll_dir(content, LV_DIR_VER);
  lv_obj_remove_flag(content, LV_OBJ_FLAG_SCROLL_ELASTIC);
  lv_obj_set_style_bg_color(content, lv_color_hex(CARD), 0);
  lv_obj_set_style_border_width(content, 0, 0);
  lv_obj_set_style_pad_all(content, 6, 0);
  notice = label(root, "Connect Wi-Fi and pair with your scale", 18, 388, 444,
                 &lv_font_montserrat_14);
  lv_obj_set_height(notice, 40);
  const char *names[] = {"Home", "Keg", "Scale", "Setup", "Update"};
  for (int i = 0; i < 5; i++)
    button(root, names[i], 8 + i * 94, 430, 88, nav, (void *)(intptr_t)i);
  build(s.ssid[0] && s.host[0] ? 0 : 3);
  bsp_display_unlock();
}
void ui_state(const State &s) {
  if (!bsp_display_lock(1000))
    return;
  bool lost_connection = current.online && !s.online;
  if (!s.online) {
    editor_valid = false;
    cal_step = 0;
  }
  current = s;
  if (lost_connection && page_id == 2)
    build(2);
  dashboard();
  bsp_display_unlock();
}
void ui_message(const char *s) {
  if (!bsp_display_lock(1000))
    return;
  message(s);
  bsp_display_unlock();
}
void ui_pair_code(const char *code) {
  if (!bsp_display_lock(1000))
    return;
  snprintf(pair_code, sizeof(pair_code), "%s", code);
  build(3);
  message("Enter this code on the scale's web page");
  bsp_display_unlock();
}
void ui_result(bool ok, const char *op, const char *error) {
  if (!bsp_display_lock(1000))
    return;
  if (ok) {
    if (!strcmp(op, "save")) {
      build(0);
    } else if (!strcmp(op, "begin_calibration")) {
      cal_step = 1;
      build(2);
    } else if (!strcmp(op, "tare")) {
      cal_step = 2;
      build(2);
    } else if (!strcmp(op, "calibrate")) {
      cal_step = 0;
      build(0);
    }
    message(!strcmp(op, "save") ? "Keg information saved on scale"
                                : "Scale confirmed the operation");
  } else
    message(error);
  bsp_display_unlock();
}
void ui_discovered(const char *host) {
  if (!bsp_display_lock(1000))
    return;
  if (page_id == 3 && fields[2])
    lv_textarea_set_text(fields[2], host);
  message("Scale found. Save and connect.");
  bsp_display_unlock();
}
void ui_networks(const char *options) {
  if (!bsp_display_lock(1000))
    return;
  if (page_id == 3 && networks)
    lv_dropdown_set_options(networks, options);
  message("Choose your Wi-Fi network");
  bsp_display_unlock();
}
void ui_update_progress(int percent) {
  char b[80];
  snprintf(b, sizeof(b), "Updating touchscreen: %d%% — keep power connected",
           percent);
  ui_message(b);
}

void ui_paired(void) {
  if (!bsp_display_lock(1000))
    return;
  bool first = pair_code[0] != 0;
  pair_code[0] = 0;
  if (first)
    build(0);
  bsp_display_unlock();
}
