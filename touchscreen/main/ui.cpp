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

// Implemented by home_layout.cpp. Home is rendered synchronously by the
// custom Dashboard/Glass renderer instead of the legacy Home widgets.
void touchscreen_home_render_now();
namespace {
Settings initial{};
State current{};
lv_obj_t *content, *notice, *keyboard, *fields[6], *headline, *detail,
    *connection, *arc, *networks;
int page_id, cal_step;
uint32_t edit_revision;
bool editor_valid;
char pair_code[13];
char scale_hosts_ui[2][128] = {};
bool scale_paired_ui[2] = {};
uint8_t active_scale_ui = 0;
uint8_t setup_scale_slot = 0;
lv_obj_t *scale_slot_dropdown = nullptr;
lv_obj_t *scale_host_dropdown = nullptr;
lv_obj_t *scale_manual_label = nullptr;
lv_obj_t *scale_manual_host = nullptr;
lv_obj_t *home_nav_button = nullptr;
char discovered_scale_options[800] = "Manual IP / hostname...";
bool ui_initialized = false;
const uint32_t BG = 0x101c26, CARD = 0x203441, ACCENT = 0x54d6bf,
               TEXT = 0xf2f6f8;
void build(int page);
void focus(lv_event_t *e);
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
lv_obj_t *button(lv_obj_t *parent, const char *s, int x, int y, int width,
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
  return o;
}

bool two_scales_ready() {
  return scale_hosts_ui[0][0] && scale_paired_ui[0] && scale_hosts_ui[1][0] &&
         scale_paired_ui[1];
}

void update_home_nav_button() {
  if (!home_nav_button)
    return;
  lv_obj_t *text = lv_obj_get_child(home_nav_button, 0);
  if (!text || !lv_obj_check_type(text, &lv_label_class))
    return;
  const bool can_switch = page_id == 0 && two_scales_ready();
  lv_label_set_text(text, can_switch ? "Switch\nScale" : "Home");
  lv_obj_set_style_text_align(text, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_center(text);
}

bool is_manual_host_option(const char *text) {
  return text && !strcmp(text, "Manual IP / hostname...");
}

void set_manual_host_visible(bool visible) {
  if (scale_manual_label) {
    if (visible)
      lv_obj_remove_flag(scale_manual_label, LV_OBJ_FLAG_HIDDEN);
    else
      lv_obj_add_flag(scale_manual_label, LV_OBJ_FLAG_HIDDEN);
  }
  if (scale_manual_host) {
    if (visible)
      lv_obj_remove_flag(scale_manual_host, LV_OBJ_FLAG_HIDDEN);
    else
      lv_obj_add_flag(scale_manual_host, LV_OBJ_FLAG_HIDDEN);
  }
}

void select_host_option_for_slot(bool prefer_discovered) {
  if (!scale_host_dropdown)
    return;

  const char *saved = scale_hosts_ui[setup_scale_slot];
  const char *other = scale_hosts_ui[setup_scale_slot == 0 ? 1 : 0];
  const uint16_t option_count = lv_dropdown_get_option_count(scale_host_dropdown);
  int saved_index = -1;
  int first_available = -1;
  int manual_index = -1;
  char option[128];

  for (uint16_t i = 0; i < option_count; ++i) {
    lv_dropdown_set_selected(scale_host_dropdown, i);
    lv_dropdown_get_selected_str(scale_host_dropdown, option, sizeof(option));
    if (is_manual_host_option(option)) {
      manual_index = i;
      continue;
    }
    if (saved[0] && !strcmp(option, saved))
      saved_index = i;
    if (first_available < 0 && (!other[0] || strcmp(option, other)))
      first_available = i;
  }

  int chosen = manual_index >= 0 ? manual_index : 0;
  if (saved_index >= 0)
    chosen = saved_index;
  else if (prefer_discovered && first_available >= 0)
    chosen = first_available;

  lv_dropdown_set_selected(scale_host_dropdown, (uint16_t)chosen);
  lv_dropdown_get_selected_str(scale_host_dropdown, option, sizeof(option));
  const bool manual = is_manual_host_option(option);
  if (manual && scale_manual_host)
    lv_textarea_set_text(scale_manual_host, saved);
  set_manual_host_visible(manual);
}

void load_saved_host_options() {
  if (!scale_host_dropdown)
    return;
  char options[320] = {};
  const char *saved = scale_hosts_ui[setup_scale_slot];
  if (saved[0]) {
    snprintf(options, sizeof(options), "%s\nManual IP / hostname...", saved);
  } else {
    snprintf(options, sizeof(options), "Manual IP / hostname...");
  }
  lv_dropdown_set_options(scale_host_dropdown, options);
  select_host_option_for_slot(false);
}

void scale_host_selection_changed(lv_event_t *event) {
  char selected[128];
  lv_dropdown_get_selected_str((lv_obj_t *)lv_event_get_target(event),
                               selected, sizeof(selected));
  const bool manual = is_manual_host_option(selected);
  if (manual && scale_manual_host && !lv_textarea_get_text(scale_manual_host)[0])
    lv_textarea_set_text(scale_manual_host, scale_hosts_ui[setup_scale_slot]);
  set_manual_host_visible(manual);
}

void scale_slot_changed(lv_event_t *event) {
  const uint16_t selected =
      lv_dropdown_get_selected((lv_obj_t *)lv_event_get_target(event));
  setup_scale_slot = selected == 1 ? 1 : 0;
  if (scale_host_dropdown &&
      strcmp(discovered_scale_options, "Manual IP / hostname...")) {
    lv_dropdown_set_options(scale_host_dropdown, discovered_scale_options);
    select_host_option_for_slot(true);
  } else {
    load_saved_host_options();
  }
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
void nav(lv_event_t *e) {
  const int target = (int)(intptr_t)lv_event_get_user_data(e);
  if (target == 0 && page_id == 0 && two_scales_ready()) {
    if (submit("switch_scale", nullptr))
      message("Switching scale...");
    return;
  }
  build(target);
}
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
  auto o = cJSON_CreateObject();
  cJSON_AddNumberToObject(o, "slot", setup_scale_slot);
  submit("discover", o);
  cJSON_Delete(o);
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
  char selected[128] = {};
  if (scale_host_dropdown)
    lv_dropdown_get_selected_str(scale_host_dropdown, selected,
                                 sizeof(selected));
  const char *host =
      is_manual_host_option(selected)
          ? (scale_manual_host ? lv_textarea_get_text(scale_manual_host) : "")
          : selected;

  auto o = cJSON_CreateObject();
  cJSON_AddNumberToObject(o, "slot", setup_scale_slot);
  cJSON_AddStringToObject(o, "ssid", lv_textarea_get_text(fields[0]));
  cJSON_AddStringToObject(o, "password", lv_textarea_get_text(fields[1]));
  cJSON_AddStringToObject(o, "host", host);
  cJSON_AddNumberToObject(o, "brightness", lv_slider_get_value(fields[3]));
  submit("settings", o);
  cJSON_Delete(o);
}
void brightness(lv_event_t *e) {
  bsp_display_brightness_set(
      lv_slider_get_value((lv_obj_t *)lv_event_get_target(e)));
}
void forget_yes(lv_event_t *) {
  auto o = cJSON_CreateObject();
  cJSON_AddNumberToObject(o, "slot", setup_scale_slot);
  submit("forget", o);
  cJSON_Delete(o);
  build(3);
}
void forget(lv_event_t *) {
  dismiss_keyboard();
  // A Wi-Fi scan/discovery may complete while this confirmation is visible.
  // Do not let its result write into deleted setup widgets.
  memset(fields, 0, sizeof(fields));
  networks = nullptr;
  scale_slot_dropdown = nullptr;
  scale_host_dropdown = nullptr;
  scale_manual_label = nullptr;
  scale_manual_host = nullptr;
  lv_obj_clean(content);
  label(content, "Remove scale pairing?", 8, 12, 424,
        &lv_font_montserrat_24);
  label(content,
        "This removes the pairing from both the touchscreen and the scale. "
        "Use Add touchscreen on the scale when you are ready to pair again.",
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
    if (!current.online) {
      lv_label_set_text(detail, "--");
      lv_arc_set_value(arc, 0);
      lv_label_set_text(connection, "Not connected");
      return;
    }
    if (current.ready)
      lv_label_set_text_fmt(detail, "%.0f", (double)floorf(current.servings));
    else
      lv_label_set_text(detail, "--");
    lv_arc_set_value(arc, current.ready ? (int)current.percent : 0);
    lv_label_set_text_fmt(connection,
                          "Connected\n%.2f gal | %.1f oz | %.2f lb | %s",
                          (double)current.gallons, (double)current.serving,
                          (double)current.weight,
                          current.stable ? "Stable" : "Settling");

  } else if (page_id == 2) {
    lv_label_set_text_fmt(
        headline, "Scale: %.3f lb   %s", (double)current.weight,
        current.online ? (current.stable ? "Stable" : "Settling")
                       : "Not connected");
  }
}
void build(int page) {
  dismiss_keyboard();
  page_id = page;
  update_home_nav_button();
  lv_obj_clean(content);
  headline = detail = connection = arc = networks = nullptr;
  scale_slot_dropdown = nullptr;
  scale_host_dropdown = nullptr;
  scale_manual_label = nullptr;
  scale_manual_host = nullptr;
  memset(fields, 0, sizeof(fields));
  if (page == 0) {
    // Identify the shared content container as Home without building the old
    // arc/servings/Replace-keg UI. The custom renderer runs before this
    // navigation callback returns, so there is no legacy Home frame to flash.
    auto marker =
        label(content, "__TOUCH_HOME__", 0, 0, 1, &lv_font_montserrat_14);
    lv_obj_add_flag(marker, LV_OBJ_FLAG_HIDDEN);
    touchscreen_home_render_now();
  } else if (page == 1) {
    edit_revision = current.revision;
    editor_valid = current.online;
    label(content, "Keg information", 8, 0, 424, &lv_font_montserrat_24);
    if (!current.online) {
      label(content, "Not connected", 8, 56, 424, &lv_font_montserrat_24);
      label(content,
            "Connect to the scale before viewing or editing keg information.",
            8, 105, 424, &lv_font_montserrat_18);
      return;
    }
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

    setup_scale_slot = active_scale_ui;
    int y = pair_code[0] ? 135 : 45;
    label(content, "Scale connection", 8, y, 424, &lv_font_montserrat_16);
    scale_slot_dropdown = lv_dropdown_create(content);
    lv_obj_set_pos(scale_slot_dropdown, 8, y + 25);
    lv_obj_set_size(scale_slot_dropdown, 424, 46);
    char scale_options[128];
    snprintf(scale_options, sizeof(scale_options),
             "Scale 1%s\nScale 2%s",
             scale_paired_ui[0] ? " - Paired"
                                : (scale_hosts_ui[0][0] ? " - Configured"
                                                        : " - Not configured"),
             scale_paired_ui[1] ? " - Paired"
                                : (scale_hosts_ui[1][0] ? " - Configured"
                                                        : " - Not configured"));
    lv_dropdown_set_options(scale_slot_dropdown, scale_options);
    lv_dropdown_set_selected(scale_slot_dropdown, setup_scale_slot);
    lv_obj_add_event_cb(scale_slot_dropdown, scale_slot_changed,
                        LV_EVENT_VALUE_CHANGED, nullptr);

    y += 82;
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

    label(content, "Scale hostname or IP", 8, y + 285, 424,
          &lv_font_montserrat_16);
    scale_host_dropdown = lv_dropdown_create(content);
    lv_obj_set_pos(scale_host_dropdown, 8, y + 310);
    lv_obj_set_size(scale_host_dropdown, 424, 46);
    lv_obj_add_event_cb(scale_host_dropdown, scale_host_selection_changed,
                        LV_EVENT_VALUE_CHANGED, nullptr);

    scale_manual_label =
        label(content, "Manual IP / hostname", 8, y + 365, 424,
              &lv_font_montserrat_16);
    scale_manual_host = lv_textarea_create(content);
    lv_obj_set_pos(scale_manual_host, 8, y + 390);
    lv_obj_set_size(scale_manual_host, 424, 46);
    lv_textarea_set_one_line(scale_manual_host, true);
    lv_textarea_set_max_length(scale_manual_host, 127);
    lv_textarea_set_text(scale_manual_host, scale_hosts_ui[setup_scale_slot]);
    lv_obj_remove_flag(scale_manual_host, LV_OBJ_FLAG_SCROLL_ON_FOCUS);
    lv_obj_add_event_cb(scale_manual_host, focus, LV_EVENT_SHORT_CLICKED,
                        nullptr);

    load_saved_host_options();

    label(content,
          "Use Find scale to discover every scale on this Wi-Fi network, or "
          "choose Manual IP / hostname. Open Add touchscreen on the selected "
          "scale before first pairing.",
          8, y + 455, 424, &lv_font_montserrat_16);

    label(content, "Brightness", 8, y + 535, 424);
    fields[3] = lv_slider_create(content);
    lv_obj_set_pos(fields[3], 18, y + 578);
    lv_obj_set_size(fields[3], 404, 15);
    lv_slider_set_range(fields[3], 10, 100);
    lv_slider_set_value(fields[3], initial.brightness, LV_ANIM_OFF);
    lv_obj_add_event_cb(fields[3], brightness, LV_EVENT_VALUE_CHANGED, nullptr);

    button(content, "Save and connect", 8, y + 615, 424, save_settings);
    button(content, "Remove selected scale pairing", 8, y + 675, 424, forget);
  } else {
    label(content, "Firmware & diagnostics", 8, 0, 424, &lv_font_montserrat_24);
    char b[300];
    snprintf(b, sizeof(b),
             "Touchscreen: %s\nScale: %s\nWi-Fi protocol: 1\nConnection: "
             "%s\n\nHardware: Waveshare 4B\nThis update is for the "
             "touchscreen. Update the scale from its web page.",
             esp_app_get_description()->version, current.firmware,
             current.online ? "Connected" : "Not connected");
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
  for (int i = 0; i < 5; i++) {
    lv_obj_t *nav_button =
        button(root, names[i], 8 + i * 94, 430, 88, nav,
               (void *)(intptr_t)i);
    if (i == 0)
      home_nav_button = nav_button;
  }
  ui_initialized = true;
  build(s.ssid[0] && scale_hosts_ui[active_scale_ui][0] ? 0 : 3);
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
  if (lost_connection && (page_id == 0 || page_id == 1 || page_id == 2))
    build(page_id);
  else
    dashboard();
  if (!s.online && (page_id == 0 || page_id == 1))
    message("Not connected");
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
  char options[320];
  if (host && host[0])
    snprintf(options, sizeof(options), "%s\nManual IP / hostname...", host);
  else
    snprintf(options, sizeof(options), "Manual IP / hostname...");
  ui_discovered_options(options);
}

void ui_discovered_options(const char *options) {
  if (!bsp_display_lock(1000))
    return;
  snprintf(discovered_scale_options, sizeof(discovered_scale_options), "%s",
           options && options[0] ? options : "Manual IP / hostname...");
  if (page_id == 3 && scale_host_dropdown) {
    lv_dropdown_set_options(scale_host_dropdown, discovered_scale_options);
    select_host_option_for_slot(true);
  }
  message("Choose a scale from the list, or use Manual IP / hostname.");
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


void ui_scale_profiles(const char *primary_host, bool primary_paired,
                       const char *secondary_host, bool secondary_paired,
                       uint8_t active_scale) {
  snprintf(scale_hosts_ui[0], sizeof(scale_hosts_ui[0]), "%s",
           primary_host ? primary_host : "");
  snprintf(scale_hosts_ui[1], sizeof(scale_hosts_ui[1]), "%s",
           secondary_host ? secondary_host : "");
  scale_paired_ui[0] = primary_paired;
  scale_paired_ui[1] = secondary_paired;
  active_scale_ui = active_scale < 2 ? active_scale : 0;
  setup_scale_slot = active_scale_ui;

  if (!ui_initialized)
    return;
  if (!bsp_display_lock(1000))
    return;
  update_home_nav_button();
  if (page_id == 3)
    build(3);
  bsp_display_unlock();
}
