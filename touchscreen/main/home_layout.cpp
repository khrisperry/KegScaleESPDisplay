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
constexpr uint32_t COLOR_HEADER_BUTTON = 0x203441;
constexpr uint32_t COLOR_HEADER_ACCENT = 0x54d6bf;
constexpr uint32_t COLOR_METRIC_CARD = 0x202020;
constexpr uint32_t COLOR_METRIC_BORDER = 0x343434;
constexpr int GLASS_BAND_COUNT = 24;
// TOUCH_DRAWER_REFINEMENTS_V2_4
// TOUCH_SHELL_POLISH_V3
// TOUCH_LAYOUT_POLISH_V4

static lv_point_precise_t glass_outline_points[] = {
    {0, 0}, {174, 0}, {152, 280}, {22, 280}, {0, 0}};

State latest_state{};
int active_page = 0;
bool home_menu_open = false;
bool glass_home = false;
bool home_disconnected = false;
lv_obj_t *home_overlay = nullptr;
lv_obj_t *home_name = nullptr;
lv_obj_t *home_status = nullptr;
lv_obj_t *home_percent = nullptr;
lv_obj_t *home_servings = nullptr;
lv_obj_t *home_gallons = nullptr;
lv_obj_t *home_beer_weight = nullptr;
lv_obj_t *home_scale_weight = nullptr;
lv_obj_t *home_arc = nullptr;
lv_obj_t *glass_bands[GLASS_BAND_COUNT] = {};
lv_obj_t *view_button = nullptr;
lv_obj_t *view_button_label = nullptr;
lv_timer_t *customization_timer = nullptr;

void reset_home_child_refs() {
  home_name = nullptr;
  home_status = nullptr;
  home_percent = nullptr;
  home_servings = nullptr;
  home_gallons = nullptr;
  home_beer_weight = nullptr;
  home_scale_weight = nullptr;
  home_arc = nullptr;
  for (auto &band : glass_bands)
    band = nullptr;
}

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
    if (!child || child == view_button)
      continue;
    const int x = lv_obj_get_x(child);
    const int y = lv_obj_get_y(child);
    const int w = lv_obj_get_width(child);
    const int h = lv_obj_get_height(child);
    if (x >= 8 && x <= 16 && y >= 6 && y <= 55 && w >= 440 && h >= 320)
      return child;
  }
  return nullptr;
}

bool content_is_home(lv_obj_t *content) {
  if (!content)
    return false;
  const uint32_t count = lv_obj_get_child_count(content);
  for (uint32_t i = 0; i < count; ++i) {
    lv_obj_t *child = lv_obj_get_child(content, static_cast<int32_t>(i));
    if (!child || child == home_overlay)
      continue;
    if (lv_obj_check_type(child, &lv_label_class)) {
      const char *text = lv_label_get_text(child);
      if (text && !strcmp(text, "__TOUCH_HOME__"))
        return true;
    }
  }
  return false;
}

int detect_page() {
  lv_obj_t *content = find_content();
  if (!content)
    return active_page;
  if (content_is_home(content))
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
  home_disconnected = false;
  reset_home_child_refs();
}

void clear_view_button_refs(lv_event_t *) {
  view_button = nullptr;
  view_button_label = nullptr;
}

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
  if (home_disconnected)
    return;
  lv_obj_clean(overlay);
  reset_home_child_refs();
  home_disconnected = true;
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
  home_disconnected = false;

  home_name = make_label(overlay, "", 16, 10, 285, &lv_font_montserrat_20);
  lv_label_set_long_mode(home_name, LV_LABEL_LONG_DOT);
  lv_obj_set_height(home_name, 26);
  // Shift Stable/Settling left to leave clear space for the hamburger.
  home_status = make_label(overlay, "", 304, 15, 88,
                           &lv_font_montserrat_14, COLOR_GREEN);
  lv_obj_set_style_text_align(home_status, LV_TEXT_ALIGN_RIGHT, 0);

  home_arc = lv_arc_create(overlay);
  lv_obj_set_pos(home_arc, 18, 62);
  lv_obj_set_size(home_arc, 180, 180);
  lv_arc_set_rotation(home_arc, 270);
  lv_arc_set_bg_angles(home_arc, 0, 360);
  lv_arc_set_range(home_arc, 0, 100);
  lv_obj_remove_style(home_arc, nullptr, LV_PART_KNOB);
  lv_obj_remove_flag(home_arc, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_style_arc_width(home_arc, 18, LV_PART_MAIN);
  lv_obj_set_style_arc_width(home_arc, 18, LV_PART_INDICATOR);
  lv_obj_set_style_arc_color(home_arc, lv_color_hex(0x303030), LV_PART_MAIN);
  lv_obj_set_style_arc_color(home_arc, lv_color_hex(COLOR_GREEN),
                             LV_PART_INDICATOR);

  home_percent = make_label(overlay, "--", 32, 116, 152,
                            &lv_font_montserrat_48);
  lv_obj_set_style_text_align(home_percent, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_t *remaining = make_label(overlay, "REMAINING", 54, 178, 108,
                                   &lv_font_montserrat_14, COLOR_MUTED);
  lv_obj_set_style_text_align(remaining, LV_TEXT_ALIGN_CENTER, 0);

  home_servings = make_label(overlay, "--", 222, 92, 72,
                             &lv_font_montserrat_28);
  make_label(overlay, "servings left", 286, 103, 132, &lv_font_montserrat_16,
             COLOR_MUTED);
  home_gallons = make_label(overlay, "", 222, 158, 202,
                            &lv_font_montserrat_14, COLOR_MUTED);

  lv_obj_t *divider = lv_obj_create(overlay);
  lv_obj_set_pos(divider, 20, 278);
  lv_obj_set_size(divider, 402, 1);
  lv_obj_set_style_bg_color(divider, lv_color_hex(0x343434), 0);
  lv_obj_set_style_border_width(divider, 0, 0);
  lv_obj_remove_flag(divider, LV_OBJ_FLAG_SCROLLABLE);

  make_label(overlay, "BEER WEIGHT", 30, 300, 150, &lv_font_montserrat_14,
             COLOR_MUTED);
  home_beer_weight = make_label(overlay, "--", 30, 326, 150,
                                &lv_font_montserrat_24);
  make_label(overlay, "SCALE WEIGHT", 232, 300, 150, &lv_font_montserrat_14,
             COLOR_MUTED);
  home_scale_weight = make_label(overlay, "--", 232, 326, 150,
                                 &lv_font_montserrat_24);
}

lv_obj_t *glass_metric_card(lv_obj_t *overlay, int y, const char *title) {
  lv_obj_t *card = lv_obj_create(overlay);
  lv_obj_set_pos(card, 224, y);
  lv_obj_set_size(card, 202, 82);
  lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_style_bg_color(card, lv_color_hex(COLOR_METRIC_CARD), 0);
  lv_obj_set_style_border_color(card, lv_color_hex(COLOR_METRIC_BORDER), 0);
  lv_obj_set_style_border_width(card, 1, 0);
  lv_obj_set_style_radius(card, 12, 0);
  lv_obj_set_style_pad_all(card, 0, 0);
  make_label(card, title, 12, 9, 178, &lv_font_montserrat_14, COLOR_MUTED);
  return card;
}

void build_glass(lv_obj_t *overlay) {
  home_disconnected = false;
  home_name = make_label(overlay, "", 16, 6, 280, &lv_font_montserrat_20);
  lv_label_set_long_mode(home_name, LV_LABEL_LONG_DOT);
  lv_obj_set_height(home_name, 25);
  // Shift Stable/Settling left to leave clear space for the hamburger.
  home_status = make_label(overlay, "", 304, 10, 88,
                           &lv_font_montserrat_14, COLOR_GREEN);
  lv_obj_set_style_text_align(home_status, LV_TEXT_ALIGN_RIGHT, 0);

  // A tapered pint glass: wider at the rim, narrower at the base. The beer is
  // rendered as narrow horizontal bands so the liquid follows the taper at
  // every fill level instead of appearing as a rectangular block.
  constexpr int glass_center_x = 105;
  constexpr int liquid_top_y = 68;
  constexpr int band_step = 11;
  constexpr int top_inner_width = 160;
  constexpr int bottom_inner_width = 118;
  for (int i = 0; i < GLASS_BAND_COUNT; ++i) {
    const int width = top_inner_width -
                      ((top_inner_width - bottom_inner_width) * i) /
                          (GLASS_BAND_COUNT - 1);
    const int x = glass_center_x - width / 2;
    const int y = liquid_top_y + i * band_step;
    lv_obj_t *band = lv_obj_create(overlay);
    glass_bands[i] = band;
    lv_obj_set_pos(band, x, y);
    lv_obj_set_size(band, width, band_step + 1);
    lv_obj_remove_flag(band, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(band, lv_color_hex(COLOR_AMBER), 0);
    lv_obj_set_style_border_width(band, 0, 0);
    lv_obj_set_style_radius(band, 0, 0);
    lv_obj_set_style_pad_all(band, 0, 0);
  }

  lv_obj_t *outline = lv_line_create(overlay);
  lv_line_set_points(outline, glass_outline_points,
                     sizeof(glass_outline_points) /
                         sizeof(glass_outline_points[0]));
  lv_obj_set_pos(outline, 18, 58);
  lv_obj_set_style_line_color(outline, lv_color_hex(COLOR_GLASS), 0);
  lv_obj_set_style_line_width(outline, 6, 0);
  lv_obj_set_style_line_rounded(outline, true, 0);

  home_percent = make_label(overlay, "--", 30, 150, 150,
                            &lv_font_montserrat_48);
  lv_obj_set_style_text_align(home_percent, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_t *remaining = make_label(overlay, "REMAINING", 51, 210, 108,
                                   &lv_font_montserrat_14, COLOR_TEXT);
  lv_obj_set_style_text_align(remaining, LV_TEXT_ALIGN_CENTER, 0);

  lv_obj_t *card = glass_metric_card(overlay, 64, "SERVINGS LEFT");
  home_servings = make_label(card, "--", 12, 34, 178,
                             &lv_font_montserrat_28);

  card = glass_metric_card(overlay, 162, "GALLONS REMAINING");
  home_gallons = make_label(card, "--", 12, 35, 178,
                            &lv_font_montserrat_24);

  card = glass_metric_card(overlay, 260, "BEER WEIGHT");
  home_beer_weight = make_label(card, "--", 12, 35, 178,
                                &lv_font_montserrat_24);
}

void update_home_values();

void rebuild_home_for_selected_view() {
  if (!home_overlay)
    return;
  lv_obj_clean(home_overlay);
  reset_home_child_refs();
  home_disconnected = false;
  if (!latest_state.online)
    update_disconnected(home_overlay);
  else if (glass_home)
    build_glass(home_overlay);
  else
    build_dashboard(home_overlay);
}

void switch_home_view(lv_event_t *) {
  glass_home = !glass_home;
  save_preference(glass_home);
  rebuild_home_for_selected_view();
  if (view_button_label)
    lv_label_set_text(view_button_label,
                      glass_home ? "Dashboard" : "Glass");
  update_home_values();
}

void ensure_view_button() {
  if (!view_button) {
    lv_obj_t *root = lv_screen_active();
    view_button = lv_button_create(root);
    // TOUCH_DRAWER_V1: view selector now lives at the bottom-right of Home.
    lv_obj_set_pos(view_button, 304, 432);
    lv_obj_set_size(view_button, 156, 40);
    lv_obj_set_style_bg_color(view_button, lv_color_hex(COLOR_HEADER_BUTTON), 0);
    lv_obj_set_style_border_color(view_button, lv_color_hex(COLOR_HEADER_ACCENT), 0);
    lv_obj_set_style_border_width(view_button, 1, 0);
    lv_obj_set_style_radius(view_button, 9, 0);
    view_button_label = lv_label_create(view_button);
    lv_obj_set_style_text_font(view_button_label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(view_button_label, lv_color_hex(COLOR_TEXT), 0);
    lv_obj_center(view_button_label);
    lv_obj_add_event_cb(view_button, switch_home_view, LV_EVENT_CLICKED, nullptr);
    lv_obj_add_event_cb(view_button, clear_view_button_refs, LV_EVENT_DELETE, nullptr);
  }

  if (active_page == 0 && !home_menu_open) {
    lv_label_set_text(view_button_label,
                      glass_home ? "Dashboard" : "Glass");
    lv_obj_remove_flag(view_button, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(view_button);
  } else {
    lv_obj_add_flag(view_button, LV_OBJ_FLAG_HIDDEN);
  }
}

void build_home_overlay() {
  lv_obj_t *content = find_content();
  if (!content || !content_is_home(content))
    return;

  home_overlay = lv_obj_create(content);
  lv_obj_set_pos(home_overlay, -6, -6);
  lv_obj_set_size(home_overlay, 456, 394);
  lv_obj_add_flag(home_overlay, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_flag(home_overlay, LV_OBJ_FLAG_FLOATING);
  lv_obj_remove_flag(home_overlay, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_style_bg_color(home_overlay, lv_color_hex(COLOR_BG), 0);
  lv_obj_set_style_bg_opa(home_overlay, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(home_overlay, 0, 0);
  lv_obj_set_style_radius(home_overlay, 0, 0);
  lv_obj_set_style_pad_all(home_overlay, 0, 0);
  lv_obj_add_event_cb(home_overlay, clear_home_refs, LV_EVENT_DELETE, nullptr);

  home_disconnected = false;
  reset_home_child_refs();
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

  if (home_disconnected || !home_percent || !home_name) {
    rebuild_home_for_selected_view();
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
    for (auto *band : glass_bands) {
      if (band)
        lv_obj_add_flag(band, LV_OBJ_FLAG_HIDDEN);
    }
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

    const int visible_bands =
        percent == 0 ? 0 : (percent * GLASS_BAND_COUNT + 99) / 100;
    for (int i = 0; i < GLASS_BAND_COUNT; ++i) {
      if (!glass_bands[i])
        continue;
      if (i >= GLASS_BAND_COUNT - visible_bands)
        lv_obj_remove_flag(glass_bands[i], LV_OBJ_FLAG_HIDDEN);
      else
        lv_obj_add_flag(glass_bands[i], LV_OBJ_FLAG_HIDDEN);
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

void nav_observer(lv_event_t *event) {
  active_page = static_cast<int>(reinterpret_cast<intptr_t>(
      lv_event_get_user_data(event)));
  ensure_view_button();
  if (active_page == 0)
    update_home_values();
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
  ensure_view_button();
  if (active_page == 0)
    update_home_values();
}
} // namespace

void touchscreen_home_render_now() {
  active_page = 0;
  ensure_view_button();
  update_home_values();
}

void touchscreen_home_set_menu_open(bool open) {
  home_menu_open = open;
  if (!view_button)
    return;
  if (open) {
    lv_obj_add_flag(view_button, LV_OBJ_FLAG_HIDDEN);
  } else if (active_page == 0) {
    lv_obj_remove_flag(view_button, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(view_button);
  }
}

void touchscreen_ui_start_dispatch(const Settings &settings) {
  load_preference();
  active_page = settings.ssid[0] && settings.host[0] ? 0 : 3;
  ui_start(settings);
  if (!bsp_display_lock(1000))
    return;
  install_nav_observers();
  ensure_view_button();
  if (!customization_timer)
    customization_timer = lv_timer_create(customization_tick, 250, nullptr);
  if (active_page == 0)
    update_home_values();
  bsp_display_unlock();
}

void touchscreen_ui_state_dispatch(const State &state) {
  latest_state = state;
  ui_state(state);
  if (!bsp_display_lock(1000))
    return;
  active_page = detect_page();
  ensure_view_button();
  if (active_page == 0)
    update_home_values();
  bsp_display_unlock();
}
