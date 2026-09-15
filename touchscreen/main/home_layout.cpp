#include "app.h"
#include "bsp/esp32_s3_touch_lcd_4b.h"
#include "esp_log.h"
#include "lvgl.h"
#include "nvs.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace {
constexpr const char *TAG = "touch_home";
constexpr const char *kNvsNamespace = "touch_ui";
constexpr const char *kGlassKey = "glass_home";

constexpr uint32_t COLOR_BG = 0x171717;
constexpr uint32_t COLOR_TEXT = 0xf7f7f7;
constexpr uint32_t COLOR_MUTED = 0xa6a6a6;
constexpr uint32_t COLOR_GREEN = 0x2bc48a;
constexpr uint32_t COLOR_AMBER = 0xd58b12;
constexpr uint32_t COLOR_WARNING = 0xf2ad45;
constexpr uint32_t COLOR_GLASS = 0x98a8b0;
constexpr uint32_t COLOR_CREAM = 0xfff5dc;
constexpr uint32_t COLOR_DARK_TEXT = 0x17120b;

State latest_state{};
int active_page = 0;
bool glass_home = false;
lv_obj_t *home_overlay = nullptr;
lv_obj_t *home_name = nullptr;
lv_obj_t *home_status = nullptr;
lv_obj_t *home_percent = nullptr;
lv_obj_t *home_servings = nullptr;
lv_obj_t *home_gallons = nullptr;
lv_obj_t *home_beer_weight = nullptr;
lv_obj_t *home_scale_weight = nullptr;
lv_obj_t *home_arc = nullptr;
lv_obj_t *glass_fill = nullptr;
lv_obj_t *glass_switch = nullptr;
lv_timer_t *customization_timer = nullptr;

void ascii_safe(const char *source, char *dest, size_t size) {
  if (!dest || size == 0)
    return;
  dest[0] = 0;
  if (!source)
    return;
  size_t out = 0;
  for (size_t i = 0; source[i] && out + 1 < size;) {
    const unsigned char c = static_cast<unsigned char>(source[i]);
    if (c >= 0x20 && c < 0x7f) {
      dest[out++] = static_cast<char>(c);
      ++i;
      continue;
    }
    if (c < 0x80) {
      ++i;
      continue;
    }
    if (out + 1 < size)
      dest[out++] = '?';
    if ((c & 0xf8) == 0xf0)
      i += 4;
    else if ((c & 0xf0) == 0xe0)
      i += 3;
    else if ((c & 0xe0) == 0xc0)
      i += 2;
    else
      ++i;
  }
  dest[out] = 0;
}

lv_obj_t *make_label(lv_obj_t *parent, const char *text, int x, int y,
                     int width, const lv_font_t *font,
                     uint32_t color = COLOR_TEXT) {
  lv_obj_t *label = lv_label_create(parent);
  lv_label_set_text(label, text ? text : "");
  lv_obj_set_pos(label, x, y);
  lv_obj_set_width(label, width);
  lv_obj_set_style_text_font(label, font, 0);
  lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
  return label;
}

lv_obj_t *find_content() {
  lv_obj_t *root = lv_screen_active();
  const uint32_t count = lv_obj_get_child_count(root);
  for (uint32_t i = 0; i < count; ++i) {
    lv_obj_t *child = lv_obj_get_child(root, static_cast<int32_t>(i));
    if (!child)
      continue;
    const int x = lv_obj_get_x(child);
    const int y = lv_obj_get_y(child);
    const int w = lv_obj_get_width(child);
    const int h = lv_obj_get_height(child);
    if (x >= 8 && x <= 16 && y >= 45 && y <= 55 && w >= 440 && h >= 320)
      return child;
  }
  return nullptr;
}

bool content_has_home_arc(lv_obj_t *content) {
  if (!content)
    return false;
  const uint32_t count = lv_obj_get_child_count(content);
  for (uint32_t i = 0; i < count; ++i) {
    lv_obj_t *child = lv_obj_get_child(content, static_cast<int32_t>(i));
    if (!child || child == home_overlay)
      continue;
    if (lv_obj_get_width(child) == 200 && lv_obj_get_height(child) == 200 &&
        lv_obj_get_x(child) >= 110 && lv_obj_get_x(child) <= 130)
      return true;
  }
  return false;
}

int detect_page() {
  lv_obj_t *content = find_content();
  if (!content)
    return active_page;
  if (content_has_home_arc(content))
    return 0;

  const uint32_t count = lv_obj_get_child_count(content);
  for (uint32_t i = 0; i < count; ++i) {
    lv_obj_t *child = lv_obj_get_child(content, static_cast<int32_t>(i));
    if (!child)
      continue;
    const char *text = nullptr;
    if (lv_obj_check_type(child, &lv_label_class))
      text = lv_label_get_text(child);
    if (!text)
      continue;
    if (!strncmp(text, "Keg information", 15) ||
        !strncmp(text, "Not connected", 13))
      return 1;
    if (!strncmp(text, "Connection & display", 20))
      return 3;
    if (!strncmp(text, "Firmware updates", 16) ||
        !strncmp(text, "Firmware & diagnostics", 22))
      return 4;
    if (!strncmp(text, "Scale:", 6) || !strncmp(text, "Scale calibration", 17))
      return 2;
  }
  return active_page;
}

void clear_home_refs(lv_event_t *) {
  home_overlay = nullptr;
  home_name = nullptr;
  home_status = nullptr;
  home_percent = nullptr;
  home_servings = nullptr;
  home_gallons = nullptr;
  home_beer_weight = nullptr;
  home_scale_weight = nullptr;
  home_arc = nullptr;
  glass_fill = nullptr;
}

void clear_switch_ref(lv_event_t *) { glass_switch = nullptr; }

void load_preference() {
  glass_home = false;
  nvs_handle_t nvs = 0;
  if (nvs_open(kNvsNamespace, NVS_READONLY, &nvs) != ESP_OK)
    return;
  uint8_t value = 0;
  if (nvs_get_u8(nvs, kGlassKey, &value) == ESP_OK)
    glass_home = value != 0;
  nvs_close(nvs);
  ESP_LOGI(TAG, "Loaded home layout: %s", glass_home ? "glass" : "dashboard");
}

void save_preference(bool enabled) {
  nvs_handle_t nvs = 0;
  esp_err_t e = nvs_open(kNvsNamespace, NVS_READWRITE, &nvs);
  if (e == ESP_OK)
    e = nvs_set_u8(nvs, kGlassKey, enabled ? 1 : 0);
  if (e == ESP_OK)
    e = nvs_commit(nvs);
  if (nvs)
    nvs_close(nvs);
  if (e != ESP_OK)
    ESP_LOGW(TAG, "Could not save home layout: %s", esp_err_to_name(e));
  else
    ESP_LOGI(TAG, "Saved home layout: %s", enabled ? "glass" : "dashboard");
}

float beer_weight(const State &state) {
  if (state.gallons > 0.0f && state.density > 0.0f)
    return state.gallons * state.density;
  return std::max(0.0f, state.weight - state.empty);
}

void update_disconnected(lv_obj_t *overlay) {
  lv_obj_clean(overlay);
  home_name = make_label(overlay, "Keg Scale", 20, 28, 392,
                         &lv_font_montserrat_24);
  lv_obj_set_style_text_align(home_name, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_t *title = make_label(overlay, "Not connected", 20, 122, 392,
                               &lv_font_montserrat_28, COLOR_WARNING);
  lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_t *help = make_label(overlay, "Waiting for scale connection", 20, 170,
                              392, &lv_font_montserrat_18, COLOR_MUTED);
  lv_obj_set_style_text_align(help, LV_TEXT_ALIGN_CENTER, 0);
}

void build_dashboard(lv_obj_t *overlay) {
  lv_obj_t *icon = lv_obj_create(overlay);
  lv_obj_set_pos(icon, 14, 12);
  lv_obj_set_size(icon, 43, 43);
  lv_obj_remove_flag(icon, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_style_bg_color(icon, lv_color_hex(0x4a361d), 0);
  lv_obj_set_style_border_width(icon, 0, 0);
  lv_obj_set_style_radius(icon, 11, 0);
  lv_obj_t *beer = lv_obj_create(icon);
  lv_obj_set_pos(beer, 14, 9);
  lv_obj_set_size(beer, 15, 24);
  lv_obj_remove_flag(beer, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_style_bg_color(beer, lv_color_hex(COLOR_AMBER), 0);
  lv_obj_set_style_border_color(beer, lv_color_hex(0xf0b22f), 0);
  lv_obj_set_style_border_width(beer, 2, 0);
  lv_obj_set_style_radius(beer, 2, 0);

  home_name = make_label(overlay, "", 68, 10, 260, &lv_font_montserrat_20);
  lv_label_set_long_mode(home_name, LV_LABEL_LONG_DOT);
  lv_obj_set_height(home_name, 26);
  make_label(overlay, "Keg Scale", 68, 36, 170, &lv_font_montserrat_14,
             COLOR_MUTED);
  home_status = make_label(overlay, "", 320, 15, 105,
                           &lv_font_montserrat_14, COLOR_GREEN);
  lv_obj_set_style_text_align(home_status, LV_TEXT_ALIGN_RIGHT, 0);

  home_arc = lv_arc_create(overlay);
  lv_obj_set_pos(home_arc, 16, 72);
  lv_obj_set_size(home_arc, 150, 150);
  lv_arc_set_rotation(home_arc, 270);
  lv_arc_set_bg_angles(home_arc, 0, 360);
  lv_arc_set_range(home_arc, 0, 100);
  lv_obj_remove_style(home_arc, nullptr, LV_PART_KNOB);
  lv_obj_remove_flag(home_arc, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_style_arc_width(home_arc, 16, LV_PART_MAIN);
  lv_obj_set_style_arc_width(home_arc, 16, LV_PART_INDICATOR);
  lv_obj_set_style_arc_color(home_arc, lv_color_hex(0x303030), LV_PART_MAIN);
  lv_obj_set_style_arc_color(home_arc, lv_color_hex(COLOR_GREEN),
                             LV_PART_INDICATOR);

  home_percent = make_label(overlay, "--", 28, 108, 126,
                            &lv_font_montserrat_48);
  lv_obj_set_style_text_align(home_percent, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_t *remaining = make_label(overlay, "REMAINING", 42, 164, 98,
                                   &lv_font_montserrat_14, COLOR_MUTED);
  lv_obj_set_style_text_align(remaining, LV_TEXT_ALIGN_CENTER, 0);

  home_servings = make_label(overlay, "--", 192, 94, 70,
                             &lv_font_montserrat_28);
  make_label(overlay, "servings left", 256, 105, 155, &lv_font_montserrat_16,
             COLOR_MUTED);
  home_gallons = make_label(overlay, "", 192, 153, 220,
                            &lv_font_montserrat_14, COLOR_MUTED);

  lv_obj_t *divider = lv_obj_create(overlay);
  lv_obj_set_pos(divider, 20, 236);
  lv_obj_set_size(divider, 402, 1);
  lv_obj_set_style_bg_color(divider, lv_color_hex(0x343434), 0);
  lv_obj_set_style_border_width(divider, 0, 0);
  lv_obj_remove_flag(divider, LV_OBJ_FLAG_SCROLLABLE);

  make_label(overlay, "BEER WEIGHT", 30, 254, 150, &lv_font_montserrat_14,
             COLOR_MUTED);
  home_beer_weight = make_label(overlay, "--", 30, 276, 150,
                                &lv_font_montserrat_20);
  make_label(overlay, "SCALE WEIGHT", 232, 254, 150, &lv_font_montserrat_14,
             COLOR_MUTED);
  home_scale_weight = make_label(overlay, "--", 232, 276, 150,
                                 &lv_font_montserrat_20);
}

lv_obj_t *metric_card(lv_obj_t *overlay, int y, const char *subtext) {
  lv_obj_t *card = lv_obj_create(overlay);
  lv_obj_set_pos(card, 104, y);
  lv_obj_set_size(card, 244, 54);
  lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_style_bg_color(card, lv_color_hex(COLOR_CREAM), 0);
  lv_obj_set_style_border_width(card, 0, 0);
  lv_obj_set_style_radius(card, 13, 0);
  lv_obj_t *sub = make_label(card, subtext, 8, 32, 228,
                             &lv_font_montserrat_14, 0x7c4d08);
  lv_obj_set_style_text_align(sub, LV_TEXT_ALIGN_CENTER, 0);
  return card;
}

void build_glass(lv_obj_t *overlay) {
  home_name = make_label(overlay, "", 16, 6, 280, &lv_font_montserrat_20);
  lv_label_set_long_mode(home_name, LV_LABEL_LONG_DOT);
  lv_obj_set_height(home_name, 25);
  home_status = make_label(overlay, "", 310, 8, 115,
                           &lv_font_montserrat_14, COLOR_GREEN);
  lv_obj_set_style_text_align(home_status, LV_TEXT_ALIGN_RIGHT, 0);

  lv_obj_t *handle = lv_obj_create(overlay);
  lv_obj_set_pos(handle, 300, 78);
  lv_obj_set_size(handle, 105, 184);
  lv_obj_remove_flag(handle, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_style_bg_opa(handle, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_color(handle, lv_color_hex(COLOR_GLASS), 0);
  lv_obj_set_style_border_width(handle, 16, 0);
  lv_obj_set_style_radius(handle, 30, 0);

  glass_fill = lv_obj_create(overlay);
  lv_obj_set_pos(glass_fill, 71, 154);
  lv_obj_set_size(glass_fill, 253, 144);
  lv_obj_remove_flag(glass_fill, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_style_bg_color(glass_fill, lv_color_hex(COLOR_AMBER), 0);
  lv_obj_set_style_border_width(glass_fill, 0, 0);
  lv_obj_set_style_radius(glass_fill, 0, 0);

  lv_obj_t *outline = lv_obj_create(overlay);
  lv_obj_set_pos(outline, 63, 36);
  lv_obj_set_size(outline, 270, 270);
  lv_obj_remove_flag(outline, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_style_bg_opa(outline, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_color(outline, lv_color_hex(COLOR_GLASS), 0);
  lv_obj_set_style_border_width(outline, 7, 0);
  lv_obj_set_style_radius(outline, 8, 0);

  lv_obj_t *card = metric_card(overlay, 45, "REMAINING");
  home_percent = make_label(card, "--", 8, 2, 228, &lv_font_montserrat_28,
                            COLOR_DARK_TEXT);
  lv_obj_set_style_text_align(home_percent, LV_TEXT_ALIGN_CENTER, 0);

  card = metric_card(overlay, 107, "SERVINGS LEFT");
  home_servings = make_label(card, "--", 8, 4, 228, &lv_font_montserrat_24,
                             COLOR_DARK_TEXT);
  lv_obj_set_style_text_align(home_servings, LV_TEXT_ALIGN_CENTER, 0);

  card = metric_card(overlay, 169, "GALLONS REMAINING");
  home_gallons = make_label(card, "--", 8, 4, 228, &lv_font_montserrat_24,
                            COLOR_DARK_TEXT);
  lv_obj_set_style_text_align(home_gallons, LV_TEXT_ALIGN_CENTER, 0);

  card = metric_card(overlay, 231, "BEER WEIGHT");
  home_beer_weight = make_label(card, "--", 8, 4, 228,
                                &lv_font_montserrat_24, COLOR_DARK_TEXT);
  lv_obj_set_style_text_align(home_beer_weight, LV_TEXT_ALIGN_CENTER, 0);
}

void build_home_overlay() {
  lv_obj_t *content = find_content();
  if (!content || !content_has_home_arc(content))
    return;

  home_overlay = lv_obj_create(content);
  lv_obj_set_pos(home_overlay, -6, -6);
  lv_obj_set_size(home_overlay, 456, 334);
  lv_obj_add_flag(home_overlay, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_flag(home_overlay, LV_OBJ_FLAG_FLOATING);
  lv_obj_remove_flag(home_overlay, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_style_bg_color(home_overlay, lv_color_hex(COLOR_BG), 0);
  lv_obj_set_style_bg_opa(home_overlay, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(home_overlay, 0, 0);
  lv_obj_set_style_radius(home_overlay, 0, 0);
  lv_obj_set_style_pad_all(home_overlay, 0, 0);
  lv_obj_add_event_cb(home_overlay, clear_home_refs, LV_EVENT_DELETE, nullptr);

  if (!latest_state.online)
    update_disconnected(home_overlay);
  else if (glass_home)
    build_glass(home_overlay);
  else
    build_dashboard(home_overlay);

  lv_obj_move_foreground(home_overlay);
}

void update_home_values() {
  if (active_page != 0)
    return;
  if (!home_overlay) {
    build_home_overlay();
    if (!home_overlay)
      return;
  }

  if (!latest_state.online) {
    update_disconnected(home_overlay);
    return;
  }

  if (!home_percent || !home_name) {
    lv_obj_clean(home_overlay);
    if (glass_home)
      build_glass(home_overlay);
    else
      build_dashboard(home_overlay);
  }

  char name[48];
  ascii_safe(latest_state.name[0] ? latest_state.name : "Keg Scale", name,
             sizeof(name));
  lv_label_set_text(home_name, name);

  const char *status = latest_state.stable ? "Stable" : "Settling";
  lv_label_set_text(home_status, status);
  lv_obj_set_style_text_color(
      home_status,
      lv_color_hex(latest_state.stable ? COLOR_GREEN : COLOR_WARNING), 0);

  if (!latest_state.ready) {
    lv_label_set_text(home_percent, "--");
    lv_label_set_text(home_servings, "--");
    lv_label_set_text(home_gallons, "--");
    lv_label_set_text(home_beer_weight, "--");
    if (home_scale_weight)
      lv_label_set_text(home_scale_weight, "--");
    if (home_arc)
      lv_arc_set_value(home_arc, 0);
    return;
  }

  const int percent = std::clamp(static_cast<int>(std::lround(latest_state.percent)),
                                 0, 100);
  char buffer[96];
  snprintf(buffer, sizeof(buffer), "%d%%", percent);
  lv_label_set_text(home_percent, buffer);

  const int servings = std::max(0, static_cast<int>(std::floor(latest_state.servings)));
  snprintf(buffer, sizeof(buffer), "%d", servings);
  lv_label_set_text(home_servings, buffer);

  if (glass_home) {
    snprintf(buffer, sizeof(buffer), "%.2f gal", (double)latest_state.gallons);
    lv_label_set_text(home_gallons, buffer);
    snprintf(buffer, sizeof(buffer), "%.1f lb", (double)beer_weight(latest_state));
    lv_label_set_text(home_beer_weight, buffer);
    if (glass_fill) {
      constexpr int top = 43;
      constexpr int bottom = 299;
      constexpr int height = bottom - top;
      const int fill_height = std::clamp((height * percent) / 100, 0, height);
      lv_obj_set_pos(glass_fill, 71, bottom - fill_height);
      lv_obj_set_size(glass_fill, 253, fill_height);
    }
  } else {
    snprintf(buffer, sizeof(buffer), "About %.2f gallons remaining",
             (double)latest_state.gallons);
    lv_label_set_text(home_gallons, buffer);
    snprintf(buffer, sizeof(buffer), "%.1f lb", (double)beer_weight(latest_state));
    lv_label_set_text(home_beer_weight, buffer);
    snprintf(buffer, sizeof(buffer), "%.1f lb", (double)latest_state.weight);
    lv_label_set_text(home_scale_weight, buffer);
    if (home_arc)
      lv_arc_set_value(home_arc, percent);
  }
}

void layout_switch_changed(lv_event_t *event) {
  lv_obj_t *sw = static_cast<lv_obj_t *>(lv_event_get_target(event));
  glass_home = lv_obj_has_state(sw, LV_STATE_CHECKED);
  save_preference(glass_home);
}

void ensure_setup_option() {
  if (active_page != 3 || glass_switch)
    return;
  lv_obj_t *content = find_content();
  if (!content)
    return;

  int bottom = 0;
  const uint32_t count = lv_obj_get_child_count(content);
  for (uint32_t i = 0; i < count; ++i) {
    lv_obj_t *child = lv_obj_get_child(content, static_cast<int32_t>(i));
    if (!child)
      continue;
    bottom = std::max(bottom, lv_obj_get_y(child) + lv_obj_get_height(child));
  }
  const int y = bottom + 18;
  make_label(content, "Home screen", 8, y, 300, &lv_font_montserrat_18);
  make_label(content,
             "Dashboard is the default. Enable Glass view for the visual "
             "pour-level layout.",
             8, y + 30, 335, &lv_font_montserrat_14, COLOR_MUTED);
  make_label(content, "Glass view", 8, y + 76, 180, &lv_font_montserrat_16);
  glass_switch = lv_switch_create(content);
  lv_obj_set_pos(glass_switch, 365, y + 66);
  lv_obj_set_size(glass_switch, 60, 32);
  if (glass_home)
    lv_obj_add_state(glass_switch, LV_STATE_CHECKED);
  lv_obj_add_event_cb(glass_switch, layout_switch_changed,
                      LV_EVENT_VALUE_CHANGED, nullptr);
  lv_obj_add_event_cb(glass_switch, clear_switch_ref, LV_EVENT_DELETE, nullptr);
}

void nav_observer(lv_event_t *event) {
  active_page = static_cast<int>(reinterpret_cast<intptr_t>(
      lv_event_get_user_data(event)));
  if (active_page == 0)
    update_home_values();
  else if (active_page == 3)
    ensure_setup_option();
}

void install_nav_observers() {
  lv_obj_t *root = lv_screen_active();
  const uint32_t count = lv_obj_get_child_count(root);
  for (uint32_t i = 0; i < count; ++i) {
    lv_obj_t *child = lv_obj_get_child(root, static_cast<int32_t>(i));
    if (!child)
      continue;
    const int y = lv_obj_get_y(child);
    const int x = lv_obj_get_x(child);
    const int w = lv_obj_get_width(child);
    const int h = lv_obj_get_height(child);
    if (y >= 420 && y <= 440 && w >= 80 && w <= 95 && h >= 40 && h <= 55) {
      const int index = std::clamp((x - 8 + 47) / 94, 0, 4);
      lv_obj_add_event_cb(child, nav_observer, LV_EVENT_CLICKED,
                          reinterpret_cast<void *>(static_cast<intptr_t>(index)));
    }
  }
}

void customization_tick(lv_timer_t *) {
  active_page = detect_page();
  if (active_page == 0)
    update_home_values();
  else if (active_page == 3)
    ensure_setup_option();
}
} // namespace

void touchscreen_ui_start_dispatch(const Settings &settings) {
  load_preference();
  active_page = settings.ssid[0] && settings.host[0] ? 0 : 3;
  ui_start(settings);
  if (!bsp_display_lock(1000))
    return;
  install_nav_observers();
  if (!customization_timer)
    customization_timer = lv_timer_create(customization_tick, 250, nullptr);
  if (active_page == 0)
    update_home_values();
  else if (active_page == 3)
    ensure_setup_option();
  bsp_display_unlock();
}

void touchscreen_ui_state_dispatch(const State &state) {
  latest_state = state;
  ui_state(state);
  if (!bsp_display_lock(1000))
    return;
  active_page = detect_page();
  if (active_page == 0)
    update_home_values();
  bsp_display_unlock();
}
