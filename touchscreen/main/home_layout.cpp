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
constexpr uint32_t COLOR_LOW_ORANGE = 0xef6c00;
constexpr uint32_t COLOR_LOW_RED = 0xb71c1c;
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
// TOUCH_SERVING_VISUALS_V5
// TOUCH_GROWLER_POLISH_V6
// TOUCH_FOOTER_POLISH_V7

enum class ServingVessel {
  Generic,
  Can,
  Pint,
  SoloCup,
  Crowler,
  Growler,
};

static lv_point_precise_t pint_outline_points[] = {
    {0, 0}, {174, 0}, {152, 280}, {22, 280}, {0, 0}};

static lv_point_precise_t solo_outline_points[] = {
    {0, 0}, {174, 0}, {150, 280}, {24, 280}, {0, 0}};

static lv_point_precise_t growler_outline_points[] = {
    {82, 0},   {128, 0}, {128, 40}, {166, 64}, {184, 96},
    {184, 282}, {18, 282}, {18, 96}, {36, 64},  {82, 40},
    {82, 0}};

State latest_state{};
int active_page = 0;
bool home_menu_open = false;
bool home_keyboard_open = false;
bool glass_home = false;
bool home_disconnected = false;
lv_obj_t *home_overlay = nullptr;
lv_obj_t *home_name = nullptr;
lv_obj_t *home_status = nullptr;
lv_obj_t *home_percent = nullptr;
lv_obj_t *home_servings = nullptr;
lv_obj_t *home_serving_label = nullptr;
lv_obj_t *home_serving_size = nullptr;
lv_obj_t *home_gallons = nullptr;
lv_obj_t *home_beer_weight = nullptr;
lv_obj_t *home_scale_weight = nullptr;
lv_obj_t *home_tare = nullptr;
lv_obj_t *home_arc = nullptr;
lv_obj_t *glass_bands[GLASS_BAND_COUNT] = {};
ServingVessel rendered_vessel = ServingVessel::Generic;
lv_obj_t *view_button = nullptr;
lv_obj_t *view_button_label = nullptr;
lv_timer_t *customization_timer = nullptr;

// TOUCH_DISCONNECTED_QR_V1
int disconnected_discovery_mode = TOUCHSCREEN_DISCOVERY_NONE;
bool disconnected_discovery_requested = false;
char disconnected_scale_name[48] = {};
char disconnected_qr_payload[192] = {};
char disconnected_discovery_detail[160] = {};

void reset_home_child_refs() {
  home_name = nullptr;
  home_status = nullptr;
  home_percent = nullptr;
  home_servings = nullptr;
  home_serving_label = nullptr;
  home_serving_size = nullptr;
  home_gallons = nullptr;
  home_beer_weight = nullptr;
  home_scale_weight = nullptr;
  home_tare = nullptr;
  home_arc = nullptr;
  rendered_vessel = ServingVessel::Generic;
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

bool serving_size_near(float serving_size_oz, float target_oz) {
  return std::fabs(serving_size_oz - target_oz) <= 0.25f;
}

const char *serving_count_label(float serving_size_oz) {
  if (serving_size_near(serving_size_oz, 12.0f))
    return "CANS LEFT";
  if (serving_size_near(serving_size_oz, 16.0f))
    return "PINTS LEFT";
  if (serving_size_near(serving_size_oz, 20.0f))
    return "SOLO CUPS LEFT";
  if (serving_size_near(serving_size_oz, 32.0f))
    return "CROWLERS LEFT";
  if (serving_size_near(serving_size_oz, 64.0f))
    return "GROWLERS\nLEFT";
  return "SERVINGS LEFT";
}

ServingVessel vessel_for_serving(float serving_size_oz) {
  if (serving_size_near(serving_size_oz, 12.0f))
    return ServingVessel::Can;
  if (serving_size_near(serving_size_oz, 16.0f))
    return ServingVessel::Pint;
  if (serving_size_near(serving_size_oz, 20.0f))
    return ServingVessel::SoloCup;
  if (serving_size_near(serving_size_oz, 32.0f))
    return ServingVessel::Crowler;
  if (serving_size_near(serving_size_oz, 64.0f))
    return ServingVessel::Growler;
  return ServingVessel::Generic;
}

void format_serving_size(float serving_size_oz, char *buffer,
                         size_t buffer_size) {
  if (!buffer || buffer_size == 0)
    return;
  if (serving_size_oz <= 0.0f) {
    snprintf(buffer, buffer_size, "-- OZ EACH");
    return;
  }

  const int whole = static_cast<int>(std::lround(serving_size_oz));
  if (std::fabs(serving_size_oz - static_cast<float>(whole)) < 0.05f)
    snprintf(buffer, buffer_size, "%d OZ EACH", whole);
  else
    snprintf(buffer, buffer_size, "%.1f OZ EACH", (double)serving_size_oz);
}

uint8_t mix_channel(uint8_t from, uint8_t to, int numerator, int denominator) {
  if (denominator <= 0)
    return to;
  numerator = std::clamp(numerator, 0, denominator);
  return static_cast<uint8_t>(
      from + ((static_cast<int>(to) - static_cast<int>(from)) * numerator) /
                 denominator);
}

uint32_t mix_color(uint32_t from, uint32_t to, int numerator,
                   int denominator) {
  const uint8_t fr = (from >> 16) & 0xff;
  const uint8_t fg = (from >> 8) & 0xff;
  const uint8_t fb = from & 0xff;
  const uint8_t tr = (to >> 16) & 0xff;
  const uint8_t tg = (to >> 8) & 0xff;
  const uint8_t tb = to & 0xff;

  return (static_cast<uint32_t>(mix_channel(fr, tr, numerator, denominator))
          << 16) |
         (static_cast<uint32_t>(mix_channel(fg, tg, numerator, denominator))
          << 8) |
         static_cast<uint32_t>(mix_channel(fb, tb, numerator, denominator));
}

uint32_t remaining_color(int percent) {
  percent = std::clamp(percent, 0, 100);
  if (percent > 25)
    return COLOR_AMBER;
  if (percent > 10)
    return mix_color(COLOR_LOW_ORANGE, COLOR_AMBER, percent - 10, 15);
  if (percent > 5)
    return mix_color(COLOR_LOW_RED, COLOR_LOW_ORANGE, percent - 5, 5);
  return COLOR_LOW_RED;
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

void clear_disconnected_discovery() {
  disconnected_discovery_mode = TOUCHSCREEN_DISCOVERY_NONE;
  disconnected_discovery_requested = false;
  disconnected_scale_name[0] = 0;
  disconnected_qr_payload[0] = 0;
  disconnected_discovery_detail[0] = 0;
}

void request_disconnected_discovery() {
  if (touchscreen_active_scale_paired() || disconnected_discovery_requested)
    return;

  Action action{};
  snprintf(action.kind, sizeof(action.kind), "%s", "discover_qr");
  snprintf(action.body, sizeof(action.body), "%s", "{}");

  if (xQueueSend(actions, &action, 0) == pdTRUE) {
    disconnected_discovery_requested = true;
    disconnected_discovery_mode = TOUCHSCREEN_DISCOVERY_NONE;
    ESP_LOGI(TAG, "Queued unpaired-scale QR discovery");
  } else {
    disconnected_discovery_requested = false;
    disconnected_discovery_mode = TOUCHSCREEN_DISCOVERY_ERROR;
    snprintf(disconnected_discovery_detail,
             sizeof(disconnected_discovery_detail),
             "%s", "Discovery is busy. Open Setup and try Find scale.");
  }
}

void update_disconnected(lv_obj_t *overlay) {
  if (home_disconnected)
    return;

  lv_obj_clean(overlay);
  reset_home_child_refs();
  home_disconnected = true;

  const bool paired = touchscreen_active_scale_paired();

  home_name = make_label(overlay, paired ? "Keg Scale" : "Connect your scale",
                         20, 18, 392, &lv_font_montserrat_24);
  lv_obj_set_style_text_align(home_name, LV_TEXT_ALIGN_CENTER, 0);

  if (paired) {
    clear_disconnected_discovery();

    lv_obj_t *title =
        make_label(overlay, "Scale disconnected", 20, 122, 392,
                   &lv_font_montserrat_28, COLOR_WARNING);
    lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, 0);

    lv_obj_t *help =
        make_label(overlay, "Reconnecting to your paired scale...", 32, 172,
                   368, &lv_font_montserrat_18, COLOR_MUTED);
    lv_obj_set_style_text_align(help, LV_TEXT_ALIGN_CENTER, 0);

    lv_obj_t *hint =
        make_label(overlay,
                   "If the scale address or Wi-Fi changed, open Setup from the menu.",
                   42, 220, 348, &lv_font_montserrat_14, COLOR_MUTED);
    lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, 0);
    return;
  }

  if (!disconnected_discovery_requested &&
      disconnected_discovery_mode == TOUCHSCREEN_DISCOVERY_NONE) {
    request_disconnected_discovery();
  }

  if (disconnected_discovery_requested) {
    lv_obj_t *title =
        make_label(overlay, "Finding your scale...", 20, 112, 392,
                   &lv_font_montserrat_28, COLOR_WARNING);
    lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, 0);

    lv_obj_t *help =
        make_label(overlay,
                   "Checking the local network first, then Bluetooth if needed.",
                   36, 164, 360, &lv_font_montserrat_16, COLOR_MUTED);
    lv_obj_set_style_text_align(help, LV_TEXT_ALIGN_CENTER, 0);

    lv_obj_t *hint =
        make_label(overlay,
                   "Bluetooth can identify the scale even when it is not on Wi-Fi.",
                   42, 220, 348, &lv_font_montserrat_14, COLOR_MUTED);
    lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, 0);
    return;
  }

  if ((disconnected_discovery_mode == TOUCHSCREEN_DISCOVERY_WEB ||
       disconnected_discovery_mode == TOUCHSCREEN_DISCOVERY_SETUP_WIFI) &&
      disconnected_qr_payload[0]) {
    const char *scale_title =
        disconnected_scale_name[0] ? disconnected_scale_name : "Keg Scale";

    lv_obj_t *scale =
        make_label(overlay, scale_title, 42, 52, 348,
                   &lv_font_montserrat_20);
    lv_obj_set_style_text_align(scale, LV_TEXT_ALIGN_CENTER, 0);

    lv_obj_t *qr = lv_qrcode_create(overlay);
    lv_qrcode_set_size(qr, 156);
    lv_qrcode_set_dark_color(qr, lv_color_black());
    lv_qrcode_set_light_color(qr, lv_color_white());
    lv_qrcode_update(qr, disconnected_qr_payload,
                     strlen(disconnected_qr_payload));
    lv_obj_set_pos(qr, 138, 86);
    lv_obj_set_style_border_color(qr, lv_color_white(), 0);
    lv_obj_set_style_border_width(qr, 5, 0);

    const char *caption =
        disconnected_discovery_mode == TOUCHSCREEN_DISCOVERY_WEB
            ? "Scan to open the scale web page"
            : "Scan to join the scale setup Wi-Fi";

    lv_obj_t *help =
        make_label(overlay, caption, 36, 262, 360,
                   &lv_font_montserrat_16, COLOR_TEXT);
    lv_obj_set_style_text_align(help, LV_TEXT_ALIGN_CENTER, 0);

    if (disconnected_discovery_detail[0]) {
      lv_obj_t *detail =
          make_label(overlay, disconnected_discovery_detail,
                     32, 296, 368, &lv_font_montserrat_14, COLOR_MUTED);
      lv_obj_set_style_text_align(detail, LV_TEXT_ALIGN_CENTER, 0);
      lv_label_set_long_mode(detail, LV_LABEL_LONG_WRAP);
      lv_obj_set_height(detail, 58);
    }
    return;
  }

  const char *title_text = "Scale not found";
  const char *help_text =
      "Make sure the scale is powered on, then open Setup and choose Find scale.";

  if (disconnected_discovery_mode == TOUCHSCREEN_DISCOVERY_MULTIPLE) {
    title_text = "Multiple scales found";
    help_text =
        "Open Setup and choose the scale you want to pair with this display.";
  } else if (disconnected_discovery_mode == TOUCHSCREEN_DISCOVERY_ERROR) {
    title_text = "Discovery unavailable";
    help_text =
        disconnected_discovery_detail[0]
            ? disconnected_discovery_detail
            : "Open Setup and choose Find scale to try again.";
  }

  lv_obj_t *title =
      make_label(overlay, title_text, 20, 114, 392,
                 &lv_font_montserrat_28, COLOR_WARNING);
  lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, 0);

  lv_obj_t *help =
      make_label(overlay, help_text, 38, 170, 356,
                 &lv_font_montserrat_16, COLOR_MUTED);
  lv_obj_set_style_text_align(help, LV_TEXT_ALIGN_CENTER, 0);
  lv_label_set_long_mode(help, LV_LABEL_LONG_WRAP);
  lv_obj_set_height(help, 90);
}

void build_dashboard(lv_obj_t *overlay) {
  home_disconnected = false;

  home_name = make_label(overlay, "", 16, 10, 285, &lv_font_montserrat_20);
  lv_label_set_long_mode(home_name, LV_LABEL_LONG_DOT);
  lv_obj_set_height(home_name, 26);

  home_status = make_label(overlay, "", 304, 15, 88,
                           &lv_font_montserrat_14, COLOR_GREEN);
  lv_obj_set_style_text_align(home_status, LV_TEXT_ALIGN_RIGHT, 0);

  home_arc = lv_arc_create(overlay);
  lv_obj_set_pos(home_arc, 10, 56);
  lv_obj_set_size(home_arc, 194, 194);
  lv_arc_set_rotation(home_arc, 270);
  lv_arc_set_bg_angles(home_arc, 0, 360);
  lv_arc_set_range(home_arc, 0, 100);
  lv_obj_remove_style(home_arc, nullptr, LV_PART_KNOB);
  lv_obj_remove_flag(home_arc, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_style_arc_width(home_arc, 18, LV_PART_MAIN);
  lv_obj_set_style_arc_width(home_arc, 18, LV_PART_INDICATOR);
  lv_obj_set_style_arc_color(home_arc, lv_color_hex(0x303030), LV_PART_MAIN);
  lv_obj_set_style_arc_color(home_arc, lv_color_hex(COLOR_AMBER),
                             LV_PART_INDICATOR);

  home_servings = make_label(overlay, "--", 35, 96, 144,
                             &lv_font_montserrat_48);
  lv_obj_set_style_text_align(home_servings, LV_TEXT_ALIGN_CENTER, 0);

  home_serving_label = make_label(overlay, "SERVINGS LEFT", 28, 158, 158,
                                  &lv_font_montserrat_14, COLOR_TEXT);
  lv_label_set_long_mode(home_serving_label, LV_LABEL_LONG_WRAP);
  lv_obj_set_height(home_serving_label, 34);
  lv_obj_set_style_text_align(home_serving_label, LV_TEXT_ALIGN_CENTER, 0);

  home_serving_size = make_label(overlay, "-- OZ EACH", 26, 194, 162,
                                 &lv_font_montserrat_16, COLOR_TEXT);
  lv_obj_set_style_text_align(home_serving_size, LV_TEXT_ALIGN_CENTER, 0);

  home_percent = make_label(overlay, "--", 236, 88, 176,
                            &lv_font_montserrat_48);
  lv_obj_set_style_text_align(home_percent, LV_TEXT_ALIGN_LEFT, 0);
  make_label(overlay, "REMAINING", 238, 148, 170, &lv_font_montserrat_14,
             COLOR_MUTED);

  home_gallons = make_label(overlay, "", 236, 184, 176,
                            &lv_font_montserrat_16, COLOR_MUTED);
  lv_obj_set_style_text_align(home_gallons, LV_TEXT_ALIGN_LEFT, 0);

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
  home_tare = make_label(overlay, "", 232, 360, 180,
                         &lv_font_montserrat_14, COLOR_MUTED);
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

void vessel_band_geometry(ServingVessel vessel, int index, int *x, int *y,
                          int *width, int *height) {
  const int last = GLASS_BAND_COUNT - 1;

  switch (vessel) {
  case ServingVessel::Can:
    *x = 44;
    *y = 70 + index * 11;
    *width = 130;
    *height = 12;
    break;

  case ServingVessel::Crowler:
    *x = 35;
    *y = 70 + index * 11;
    *width = 148;
    *height = 12;
    break;

  case ServingVessel::Growler: {
    // Final growler liquid geometry: keep the shoulder transition, but let the
    // fill reach the bottom like the pint/can views with a slight vertical
    // overlap between bands so the base line does not show through.
    *y = 108 + index * 9;
    if (index < 5)
      *width = 112 + index * 12;
    else
      *width = 160;
    *x = 109 - *width / 2;
    *height = (index == GLASS_BAND_COUNT - 1) ? 18 : 12;
    break;
  }

  case ServingVessel::SoloCup: {
    const int top_width = 166;
    const int bottom_width = 126;
    *width = top_width - ((top_width - bottom_width) * index) / last;
    *x = 109 - *width / 2;
    *y = 70 + index * 11;
    *height = 12;
    break;
  }

  case ServingVessel::Generic:
  case ServingVessel::Pint:
  default: {
    const int top_width = 164;
    const int bottom_width = 130;
    *width = top_width - ((top_width - bottom_width) * index) / last;
    *x = 109 - *width / 2;
    *y = 70 + index * 11;
    *height = 12;
    break;
  }
  }
}

void build_vessel_outline(lv_obj_t *overlay, ServingVessel vessel) {
  if (vessel == ServingVessel::Can || vessel == ServingVessel::Crowler) {
    lv_obj_t *outline = lv_obj_create(overlay);
    const bool crowler = vessel == ServingVessel::Crowler;
    lv_obj_set_pos(outline, crowler ? 31 : 39, 58);
    lv_obj_set_size(outline, crowler ? 156 : 140, 280);
    lv_obj_remove_flag(outline, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_opa(outline, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_color(outline, lv_color_hex(COLOR_GLASS), 0);
    lv_obj_set_style_border_width(outline, 5, 0);
    lv_obj_set_style_radius(outline, crowler ? 11 : 18, 0);

    lv_obj_t *seam = lv_obj_create(overlay);
    lv_obj_set_pos(seam, crowler ? 43 : 51, 71);
    lv_obj_set_size(seam, crowler ? 132 : 116, 2);
    lv_obj_set_style_bg_color(seam, lv_color_hex(COLOR_GLASS), 0);
    lv_obj_set_style_border_width(seam, 0, 0);
    lv_obj_remove_flag(seam, LV_OBJ_FLAG_SCROLLABLE);
    return;
  }

  lv_obj_t *outline = lv_line_create(overlay);
  if (vessel == ServingVessel::Growler) {
    lv_line_set_points(outline, growler_outline_points,
                       sizeof(growler_outline_points) /
                           sizeof(growler_outline_points[0]));
    lv_obj_set_pos(outline, 8, 50);
  } else if (vessel == ServingVessel::SoloCup) {
    lv_line_set_points(outline, solo_outline_points,
                       sizeof(solo_outline_points) /
                           sizeof(solo_outline_points[0]));
    lv_obj_set_pos(outline, 22, 58);
  } else {
    lv_line_set_points(outline, pint_outline_points,
                       sizeof(pint_outline_points) /
                           sizeof(pint_outline_points[0]));
    lv_obj_set_pos(outline, 22, 58);
  }

  lv_obj_set_style_line_color(outline, lv_color_hex(COLOR_GLASS), 0);
  lv_obj_set_style_line_width(outline, 6, 0);
  lv_obj_set_style_line_rounded(outline, true, 0);

  if (vessel == ServingVessel::SoloCup) {
    lv_obj_t *rim = lv_obj_create(overlay);
    lv_obj_set_pos(rim, 39, 68);
    lv_obj_set_size(rim, 140, 10);
    lv_obj_remove_flag(rim, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_opa(rim, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_color(rim, lv_color_hex(COLOR_GLASS), 0);
    lv_obj_set_style_border_width(rim, 5, 0);
    lv_obj_set_style_radius(rim, 8, 0);

    lv_obj_t *base = lv_obj_create(overlay);
    lv_obj_set_pos(base, 72, 322);
    lv_obj_set_size(base, 74, 3);
    lv_obj_remove_flag(base, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(base, lv_color_hex(COLOR_GLASS), 0);
    lv_obj_set_style_border_width(base, 0, 0);
  }
}

void build_glass(lv_obj_t *overlay) {
  home_disconnected = false;
  home_name = make_label(overlay, "", 16, 6, 280, &lv_font_montserrat_20);
  lv_label_set_long_mode(home_name, LV_LABEL_LONG_DOT);
  lv_obj_set_height(home_name, 25);

  home_status = make_label(overlay, "", 304, 10, 88,
                           &lv_font_montserrat_14, COLOR_GREEN);
  lv_obj_set_style_text_align(home_status, LV_TEXT_ALIGN_RIGHT, 0);

  rendered_vessel = vessel_for_serving(latest_state.serving);

  for (int i = 0; i < GLASS_BAND_COUNT; ++i) {
    int x = 0, y = 0, width = 0, height = 0;
    vessel_band_geometry(rendered_vessel, i, &x, &y, &width, &height);

    lv_obj_t *band = lv_obj_create(overlay);
    glass_bands[i] = band;
    lv_obj_set_pos(band, x, y);
    lv_obj_set_size(band, width, height);
    lv_obj_remove_flag(band, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(band, lv_color_hex(COLOR_AMBER), 0);
    lv_obj_set_style_border_width(band, 0, 0);
    lv_obj_set_style_radius(band, 0, 0);
    lv_obj_set_style_pad_all(band, 0, 0);
  }

  build_vessel_outline(overlay, rendered_vessel);

  home_servings = make_label(overlay, "--", 34, 112, 150,
                             &lv_font_montserrat_48);
  lv_obj_set_style_text_align(home_servings, LV_TEXT_ALIGN_CENTER, 0);

  home_serving_label = make_label(overlay, "SERVINGS LEFT", 25, 166, 168,
                                  &lv_font_montserrat_16, COLOR_TEXT);
  lv_label_set_long_mode(home_serving_label, LV_LABEL_LONG_WRAP);
  lv_obj_set_height(home_serving_label, 48);
  lv_obj_set_style_text_line_space(home_serving_label, -2, 0);
  lv_obj_set_style_text_align(home_serving_label, LV_TEXT_ALIGN_CENTER, 0);

  lv_obj_t *serving_size_badge = lv_obj_create(overlay);
  lv_obj_set_pos(serving_size_badge, 46, 224);
  lv_obj_set_size(serving_size_badge, 126, 32);
  lv_obj_remove_flag(serving_size_badge, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_style_bg_color(serving_size_badge, lv_color_hex(0x111820), 0);
  lv_obj_set_style_bg_opa(serving_size_badge, LV_OPA_COVER, 0);
  lv_obj_set_style_border_color(serving_size_badge, lv_color_hex(0x4c5961), 0);
  lv_obj_set_style_border_width(serving_size_badge, 1, 0);
  lv_obj_set_style_radius(serving_size_badge, 16, 0);
  lv_obj_set_style_pad_all(serving_size_badge, 0, 0);

  home_serving_size =
      make_label(serving_size_badge, "-- OZ EACH", 0, 6, 126,
                 &lv_font_montserrat_16, COLOR_TEXT);
  lv_obj_set_style_text_align(home_serving_size, LV_TEXT_ALIGN_CENTER, 0);

  lv_obj_t *card = glass_metric_card(overlay, 64, "REMAINING");
  home_percent = make_label(card, "--", 12, 32, 178,
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

void tune_home_footer_layout() {
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

    // Left "home/switch keg" button.
    if (y >= 428 && y <= 455 && x <= 20 && w >= 150 && h >= 34) {
      lv_obj_set_pos(child, 12, y);
      lv_obj_set_size(child, 152, h);
      continue;
    }

    // Middle "Connected" bubble. Center it in the gap between the
    // beer-name button and Dashboard, and keep it slightly shorter so it
    // reads as a status pill rather than a third full-size button.
    if (y >= 428 && y <= 455 && x >= 160 && x <= 250 &&
        w >= 70 && w <= 125 && h >= 28 && h <= 44) {

      // Move green pill upward.
      lv_obj_set_pos(child, 179, 438);
      lv_obj_set_size(child, 112, 32);

      // Keep "Connected" text where it was visually.
      if (lv_obj_t *connected_text = lv_obj_get_child(child, 0)) {
        lv_obj_set_style_translate_y(connected_text, 3, 0);
      }

      continue;
    }
  }
}

void ensure_view_button() {
  if (!view_button) {
    lv_obj_t *root = lv_screen_active();
    view_button = lv_button_create(root);
    lv_obj_set_pos(view_button, 306, 432);
    lv_obj_set_size(view_button, 142, 40);
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

  if (active_page == 0 && !home_menu_open && !home_keyboard_open) {
    tune_home_footer_layout();
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

  if (disconnected_discovery_requested ||
      disconnected_discovery_mode != TOUCHSCREEN_DISCOVERY_NONE) {
    clear_disconnected_discovery();
  }

  if (home_disconnected || !home_percent || !home_name)
    rebuild_home_for_selected_view();

  if (glass_home &&
      vessel_for_serving(latest_state.serving) != rendered_vessel)
    rebuild_home_for_selected_view();

  char name[48];
  ascii_safe(latest_state.name[0] ? latest_state.name : "Keg Scale", name,
             sizeof(name));
  lv_label_set_text(home_name, name);

  const char *status = latest_state.stable ? "Stable" : "Settling";
  lv_label_set_text(home_status, status);
  lv_obj_set_style_text_color(
      home_status,
      lv_color_hex(latest_state.stable ? COLOR_GREEN : COLOR_WARNING), 0);

  const char *serving_label = serving_count_label(latest_state.serving);
  if (home_serving_label) {
    lv_label_set_text(home_serving_label, serving_label);
    lv_obj_set_style_text_font(
        home_serving_label,
        &lv_font_montserrat_16,
        0);
  }

  char buffer[96];
  format_serving_size(latest_state.serving, buffer, sizeof(buffer));
  if (home_serving_size)
    lv_label_set_text(home_serving_size, buffer);

  if (!latest_state.ready) {
    lv_label_set_text(home_percent, "--");
    lv_label_set_text(home_servings, "--");
    lv_label_set_text(home_gallons, "--");
    lv_label_set_text(home_beer_weight, "--");
    if (home_scale_weight)
      lv_label_set_text(home_scale_weight, "--");
    if (home_tare)
      lv_label_set_text(home_tare, "");
    if (home_arc)
      lv_arc_set_value(home_arc, 0);
    for (auto *band : glass_bands) {
      if (band)
        lv_obj_add_flag(band, LV_OBJ_FLAG_HIDDEN);
    }
    return;
  }

  const int percent = std::clamp(
      static_cast<int>(std::lround(latest_state.percent)), 0, 100);
  const uint32_t level_color = remaining_color(percent);

  snprintf(buffer, sizeof(buffer), "%d%%", percent);
  lv_label_set_text(home_percent, buffer);

  const int servings =
      std::max(0, static_cast<int>(std::floor(latest_state.servings)));
  snprintf(buffer, sizeof(buffer), "%d", servings);
  lv_label_set_text(home_servings, buffer);

  if (glass_home) {
    snprintf(buffer, sizeof(buffer), "%.2f gal", (double)latest_state.gallons);
    lv_label_set_text(home_gallons, buffer);

    snprintf(buffer, sizeof(buffer), "%.1f lb",
             (double)beer_weight(latest_state));
    lv_label_set_text(home_beer_weight, buffer);

    const int visible_bands =
        percent == 0 ? 0 : (percent * GLASS_BAND_COUNT + 99) / 100;
    for (int i = 0; i < GLASS_BAND_COUNT; ++i) {
      if (!glass_bands[i])
        continue;

      lv_obj_set_style_bg_color(glass_bands[i], lv_color_hex(level_color), 0);
      if (i >= GLASS_BAND_COUNT - visible_bands)
        lv_obj_remove_flag(glass_bands[i], LV_OBJ_FLAG_HIDDEN);
      else
        lv_obj_add_flag(glass_bands[i], LV_OBJ_FLAG_HIDDEN);
    }
  } else {
    snprintf(buffer, sizeof(buffer), "About %.2f gallons\nremaining",
             (double)latest_state.gallons);
    lv_label_set_text(home_gallons, buffer);

    snprintf(buffer, sizeof(buffer), "%.1f lb",
             (double)beer_weight(latest_state));
    lv_label_set_text(home_beer_weight, buffer);

    snprintf(buffer, sizeof(buffer), "%.1f lb", (double)latest_state.weight);
    lv_label_set_text(home_scale_weight, buffer);

    if (home_tare) {
      snprintf(buffer, sizeof(buffer), "%.1f lb keg / tare",
               (double)latest_state.empty);
      lv_label_set_text(home_tare, buffer);
    }

    if (home_arc) {
      lv_arc_set_value(home_arc, percent);
      lv_obj_set_style_arc_color(home_arc, lv_color_hex(level_color),
                                 LV_PART_INDICATOR);
    }
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

void touchscreen_home_discovery_result(
    int mode, const char *scale_name, const char *qr_payload,
    const char *detail) {
  if (!bsp_display_lock(1000))
    return;

  disconnected_discovery_requested = false;
  disconnected_discovery_mode = mode;

  snprintf(disconnected_scale_name, sizeof(disconnected_scale_name), "%s",
           scale_name ? scale_name : "");
  snprintf(disconnected_qr_payload, sizeof(disconnected_qr_payload), "%s",
           qr_payload ? qr_payload : "");
  snprintf(disconnected_discovery_detail,
           sizeof(disconnected_discovery_detail), "%s",
           detail ? detail : "");

  if (touchscreen_active_scale_paired())
    clear_disconnected_discovery();

  if (active_page == 0 && home_overlay && home_disconnected) {
    home_disconnected = false;
    update_disconnected(home_overlay);
    lv_obj_move_foreground(home_overlay);
  }

  bsp_display_unlock();
}

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
  } else if (active_page == 0 && !home_keyboard_open) {
    lv_obj_remove_flag(view_button, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(view_button);
  }
}

void touchscreen_home_set_keyboard_open(bool open) {
  home_keyboard_open = open;
  if (!view_button)
    return;

  if (open) {
    lv_obj_add_flag(view_button, LV_OBJ_FLAG_HIDDEN);
  } else if (active_page == 0 && !home_menu_open) {
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
