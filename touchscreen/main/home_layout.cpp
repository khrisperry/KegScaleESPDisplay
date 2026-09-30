#include "app.h"
#include "bsp/esp32_s3_touch_lcd_4b.h"
#include "esp_log.h"
#include "esp_app_desc.h"
#include "lvgl.h"
#include "nvs.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace {
constexpr const char *TAG = "touch_home";
constexpr const char *kNvsNamespace = "touch_ui";
constexpr const char *kGlassKey = "glass_home"; // legacy preference migration
constexpr const char *kHomeViewKey = "home_view";

constexpr uint32_t COLOR_BG = 0x101820;
constexpr uint32_t COLOR_TEXT = 0xf7f7f7;
constexpr uint32_t COLOR_MUTED = 0xb6c5ce;
constexpr uint32_t COLOR_GREEN = 0x2bc48a;
constexpr uint32_t COLOR_AMBER = 0xd58b12;
constexpr uint32_t COLOR_LOW_ORANGE = 0xef6c00;
constexpr uint32_t COLOR_LOW_RED = 0xb71c1c;
constexpr uint32_t COLOR_WARNING = 0xf2ad45;
constexpr uint32_t COLOR_GLASS = 0xb9c6cc;
constexpr uint32_t COLOR_HEADER_BUTTON = 0x173b55;
constexpr uint32_t COLOR_HEADER_ACCENT = 0x55b7e8;
constexpr uint32_t COLOR_METRIC_CARD = 0x182a37;
constexpr uint32_t COLOR_METRIC_BORDER = 0x2b4b5d;
constexpr int GLASS_BAND_COUNT = 24;
// TOUCH_DRAWER_REFINEMENTS_V2_4
// TOUCH_SHELL_POLISH_V3
// TOUCH_LAYOUT_POLISH_V4
// TOUCH_SERVING_VISUALS_V5
// TOUCH_GROWLER_POLISH_V6
// TOUCH_FOOTER_POLISH_V7
// TOUCH_MULTI_VIEW_V8
// TOUCH_VISUAL_FIDELITY_V9
// TOUCH_CLEAN_LAYOUT_V10
// TOUCH_REFINED_SPACING_V11
// TOUCH_ICON_PASS_V12
// TOUCH_HARDWARE_TWEAKS_V13
// TOUCH_FULL_WIDTH_LAYOUT_V14
// TOUCH_VERTICAL_BALANCE_V15
// TOUCH_KEG_CONTINUOUS_FILL_V16
// TOUCH_KEG_EDGE_ALIGNMENT_V17
// TOUCH_NOTICE_CLEANUP_V18

enum class ServingVessel {
  Generic,
  Can,
  Pint,
  SoloCup,
  Crowler,
  Growler,
};

enum class HomeView : uint8_t {
  Glass = 0,
  Dashboard = 1,
  Minimal = 2,
  Gauge = 3,
  KegLevel = 4,
  Service = 5,
  Count = 6,
};

const char *home_view_name(HomeView view) {
  switch (view) {
  case HomeView::Glass:
    return "Glass";
  case HomeView::Dashboard:
    return "Dashboard";
  case HomeView::Minimal:
    return "Minimal";
  case HomeView::Gauge:
    return "Gauge";
  case HomeView::KegLevel:
    return "Keg Level";
  case HomeView::Service:
    return "Service";
  default:
    return "Dashboard";
  }
}

HomeView next_home_view(HomeView view) {
  const uint8_t next =
      (static_cast<uint8_t>(view) + 1U) % static_cast<uint8_t>(HomeView::Count);
  return static_cast<HomeView>(next);
}

HomeView previous_home_view(HomeView view) {
  const uint8_t count = static_cast<uint8_t>(HomeView::Count);
  const uint8_t current = static_cast<uint8_t>(view);
  return static_cast<HomeView>((current + count - 1U) % count);
}

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
HomeView home_view = HomeView::Dashboard;
bool home_disconnected = false;
lv_obj_t *home_overlay = nullptr;
lv_obj_t *home_name = nullptr;
lv_obj_t *home_status = nullptr;
lv_obj_t *home_status_dot = nullptr;
lv_obj_t *home_percent = nullptr;
lv_obj_t *home_servings = nullptr;
lv_obj_t *home_serving_label = nullptr;
lv_obj_t *home_serving_size = nullptr;
lv_obj_t *home_gallons = nullptr;
lv_obj_t *home_beer_weight = nullptr;
lv_obj_t *home_scale_weight = nullptr;
lv_obj_t *home_tare = nullptr;
lv_obj_t *home_arc = nullptr;
lv_obj_t *home_progress = nullptr;
lv_obj_t *home_capacity = nullptr;
lv_obj_t *service_values[11] = {};
lv_obj_t *glass_bands[GLASS_BAND_COUNT] = {};
lv_obj_t *keg_fill = nullptr;
constexpr int KEG_INNER_WIDTH = 180;
constexpr int KEG_INNER_HEIGHT = 258;
ServingVessel rendered_vessel = ServingVessel::Generic;
lv_obj_t *view_button = nullptr;
lv_obj_t *view_button_label = nullptr;
lv_obj_t *view_prev_button = nullptr;
lv_obj_t *view_next_button = nullptr;
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
  home_status_dot = nullptr;
  home_percent = nullptr;
  home_servings = nullptr;
  home_serving_label = nullptr;
  home_serving_size = nullptr;
  home_gallons = nullptr;
  home_beer_weight = nullptr;
  home_scale_weight = nullptr;
  home_tare = nullptr;
  home_arc = nullptr;
  home_progress = nullptr;
  home_capacity = nullptr;
  for (auto &value : service_values)
    value = nullptr;
  rendered_vessel = ServingVessel::Generic;
  keg_fill = nullptr;
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
    return "GROWLERS LEFT";
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
  view_prev_button = nullptr;
  view_next_button = nullptr;
}

void load_preference() {
  home_view = HomeView::Dashboard;
  nvs_handle_t nvs = 0;
  if (nvs_open(kNvsNamespace, NVS_READONLY, &nvs) != ESP_OK)
    return;

  uint8_t value = 0;
  if (nvs_get_u8(nvs, kHomeViewKey, &value) == ESP_OK &&
      value < static_cast<uint8_t>(HomeView::Count)) {
    home_view = static_cast<HomeView>(value);
  } else if (nvs_get_u8(nvs, kGlassKey, &value) == ESP_OK) {
    // Preserve the original two-view preference when first installing this
    // multi-view build.
    home_view = value ? HomeView::Glass : HomeView::Dashboard;
  }

  nvs_close(nvs);
  ESP_LOGI(TAG, "Loaded home layout: %s", home_view_name(home_view));
}

void save_preference(HomeView view) {
  nvs_handle_t nvs = 0;
  esp_err_t e = nvs_open(kNvsNamespace, NVS_READWRITE, &nvs);
  if (e == ESP_OK)
    e = nvs_set_u8(nvs, kHomeViewKey, static_cast<uint8_t>(view));
  if (e == ESP_OK)
    e = nvs_commit(nvs);
  if (nvs)
    nvs_close(nvs);
  if (e != ESP_OK)
    ESP_LOGW(TAG, "Could not save home layout: %s", esp_err_to_name(e));
  else
    ESP_LOGI(TAG, "Saved home layout: %s", home_view_name(view));
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



lv_obj_t *shape(lv_obj_t *parent, int x, int y, int width, int height,
                uint32_t color, int radius = 0) {
  lv_obj_t *o = lv_obj_create(parent);
  lv_obj_set_pos(o, x, y);
  lv_obj_set_size(o, width, height);
  lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_style_bg_color(o, lv_color_hex(color), 0);
  lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(o, 0, 0);
  lv_obj_set_style_radius(o, radius, 0);
  lv_obj_set_style_pad_all(o, 0, 0);
  return o;
}

lv_obj_t *outline_shape(lv_obj_t *parent, int x, int y, int width, int height,
                        uint32_t color, int border, int radius) {
  lv_obj_t *o = lv_obj_create(parent);
  lv_obj_set_pos(o, x, y);
  lv_obj_set_size(o, width, height);
  lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_style_bg_opa(o, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_color(o, lv_color_hex(color), 0);
  lv_obj_set_style_border_width(o, border, 0);
  lv_obj_set_style_radius(o, radius, 0);
  lv_obj_set_style_pad_all(o, 0, 0);
  return o;
}


enum class MetricGlyph : uint8_t {
  None,
  Serving,
  Gallons,
  BeerWeight,
  Keg,
  ServingSize,
  ScaleWeight,
  EmptyKeg,
  Firmware,
  Display,
  Calibration,
};

lv_obj_t *icon_root(lv_obj_t *parent, int x, int y, int size = 20) {
  lv_obj_t *root = lv_obj_create(parent);
  lv_obj_set_pos(root, x, y);
  lv_obj_set_size(root, size, size);
  lv_obj_remove_flag(root, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_remove_flag(root, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_style_bg_opa(root, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(root, 0, 0);
  lv_obj_set_style_pad_all(root, 0, 0);
  return root;
}

lv_obj_t *icon_line(lv_obj_t *parent, const lv_point_precise_t *points,
                    uint32_t count, uint32_t color, int width = 2) {
  lv_obj_t *line = lv_line_create(parent);
  lv_line_set_points(line, points, count);
  lv_obj_set_style_line_color(line, lv_color_hex(color), 0);
  lv_obj_set_style_line_width(line, width, 0);
  lv_obj_set_style_line_rounded(line, true, 0);
  return line;
}

void draw_vessel_icon(lv_obj_t *root, ServingVessel vessel, uint32_t color,
                      bool measurement_marks = false) {
  // These outlines intentionally use only 2 px strokes and a 20 px canvas so
  // the hardware rendering stays close to the approved icon sheet.
  static lv_point_precise_t pint[] = {
      {4, 3}, {16, 3}, {15, 18}, {5, 18}, {4, 3}};
  static lv_point_precise_t cup[] = {
      {3, 4}, {17, 4}, {15, 18}, {5, 18}, {3, 4}};
  static lv_point_precise_t growler[] = {
      {8, 2}, {12, 2}, {12, 5}, {15, 8}, {16, 11},
      {16, 18}, {4, 18}, {4, 11}, {5, 8}, {8, 5}, {8, 2}};

  switch (vessel) {
  case ServingVessel::Can:
    outline_shape(root, 5, 2, 10, 17, color, 2, 3);
    shape(root, 6, 5, 8, 2, color, 1);
    break;
  case ServingVessel::Crowler:
    outline_shape(root, 4, 2, 12, 17, color, 2, 3);
    shape(root, 5, 5, 10, 2, color, 1);
    break;
  case ServingVessel::Growler:
    icon_line(root, growler, sizeof(growler) / sizeof(growler[0]), color);
    break;
  case ServingVessel::SoloCup:
    icon_line(root, cup, sizeof(cup) / sizeof(cup[0]), color);
    shape(root, 5, 6, 10, 2, color, 1);
    break;
  case ServingVessel::Generic:
  case ServingVessel::Pint:
  default:
    icon_line(root, pint, sizeof(pint) / sizeof(pint[0]), color);
    shape(root, 5, 6, 10, 2, color, 1);
    break;
  }

  if (measurement_marks) {
    shape(root, 11, 9, 4, 2, color, 1);
    shape(root, 11, 13, 3, 2, color, 1);
  }
}

lv_obj_t *draw_metric_icon(lv_obj_t *parent, MetricGlyph glyph, int x, int y,
                           uint32_t color = COLOR_HEADER_ACCENT,
                           ServingVessel vessel = ServingVessel::Generic) {
  if (glyph == MetricGlyph::None)
    return nullptr;

  lv_obj_t *root = icon_root(parent, x, y, 20);

  static lv_point_precise_t droplet[] = {
      {10, 2}, {14, 7}, {17, 12}, {17, 15}, {15, 18},
      {12, 19}, {8, 19}, {5, 18}, {3, 15}, {3, 12},
      {6, 7}, {10, 2}};
  static lv_point_precise_t scale_pan[] = {
      {3, 7}, {17, 7}, {18, 11}, {17, 14}, {3, 14}, {2, 11}, {3, 7}};
  static lv_point_precise_t target_h[] = {{2, 10}, {18, 10}};
  static lv_point_precise_t target_v[] = {{10, 2}, {10, 18}};

  switch (glyph) {
  case MetricGlyph::Serving:
    draw_vessel_icon(root, vessel, color, false);
    break;

  case MetricGlyph::Gallons:
    icon_line(root, droplet, sizeof(droplet) / sizeof(droplet[0]), color);
    break;

  case MetricGlyph::BeerWeight:
    icon_line(root, scale_pan, sizeof(scale_pan) / sizeof(scale_pan[0]), color);
    shape(root, 4, 15, 3, 3, color, 1);
    shape(root, 13, 15, 3, 3, color, 1);
    break;

  case MetricGlyph::Keg:
    outline_shape(root, 4, 2, 12, 17, color, 2, 4);
    shape(root, 3, 6, 14, 2, color, 1);
    shape(root, 3, 12, 14, 2, color, 1);
    break;

  case MetricGlyph::ServingSize:
    draw_vessel_icon(root, vessel, color, true);
    break;

  case MetricGlyph::ScaleWeight:
    outline_shape(root, 7, 2, 7, 10, color, 2, 2);
    shape(root, 6, 5, 9, 2, color, 1);
    shape(root, 2, 14, 16, 3, color, 1);
    shape(root, 3, 17, 3, 2, color, 1);
    shape(root, 14, 17, 3, 2, color, 1);
    break;

  case MetricGlyph::EmptyKeg:
    outline_shape(root, 4, 2, 12, 17, color, 2, 4);
    shape(root, 3, 6, 14, 2, color, 1);
    shape(root, 3, 12, 14, 2, color, 1);
    break;

  case MetricGlyph::Firmware:
    outline_shape(root, 5, 5, 10, 10, color, 2, 2);
    for (int i = 0; i < 3; ++i) {
      const int p = 6 + i * 4;
      shape(root, p, 1, 2, 4, color, 1);
      shape(root, p, 15, 2, 4, color, 1);
      shape(root, 1, p, 4, 2, color, 1);
      shape(root, 15, p, 4, 2, color, 1);
    }
    break;

  case MetricGlyph::Display:
    outline_shape(root, 2, 3, 16, 12, color, 2, 2);
    shape(root, 6, 17, 8, 2, color, 1);
    shape(root, 8, 15, 4, 3, color, 1);
    break;

  case MetricGlyph::Calibration:
    outline_shape(root, 4, 4, 12, 12, color, 2, 10);
    outline_shape(root, 7, 7, 6, 6, color, 1, 10);
    icon_line(root, target_h, sizeof(target_h) / sizeof(target_h[0]), color);
    icon_line(root, target_v, sizeof(target_v) / sizeof(target_v[0]), color);
    break;

  case MetricGlyph::None:
  default:
    break;
  }

  return root;
}

lv_obj_t *metric_card(lv_obj_t *overlay, int x, int y, int width,
                      int height, const char *title,
                      uint32_t accent = COLOR_HEADER_ACCENT,
                      MetricGlyph glyph = MetricGlyph::None,
                      ServingVessel vessel = ServingVessel::Generic) {
  lv_obj_t *card = lv_obj_create(overlay);
  lv_obj_set_pos(card, x, y);
  lv_obj_set_size(card, width, height);
  lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_style_bg_color(card, lv_color_hex(COLOR_METRIC_CARD), 0);
  lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
  lv_obj_set_style_border_color(card, lv_color_hex(COLOR_METRIC_BORDER), 0);
  lv_obj_set_style_border_width(card, 1, 0);
  lv_obj_set_style_radius(card, 11, 0);
  lv_obj_set_style_pad_all(card, 0, 0);

  shape(card, 0, 12, 4, height - 24, accent, 2);
  const int title_x = glyph == MetricGlyph::None ? 14 : 40;
  if (glyph != MetricGlyph::None)
    draw_metric_icon(card, glyph, 14, 7, accent, vessel);
  if (title && title[0])
    make_label(card, title, title_x, 9, width - title_x - 12,
               &lv_font_montserrat_14, COLOR_MUTED);
  return card;
}

void build_home_header(lv_obj_t *overlay) {
  home_name = make_label(overlay, "", 16, 8, 280, &lv_font_montserrat_20);
  lv_label_set_long_mode(home_name, LV_LABEL_LONG_DOT);
  lv_obj_set_height(home_name, 26);

  home_status_dot = shape(overlay, 304, 17, 8, 8, COLOR_GREEN, 8);
  home_status = make_label(overlay, "", 318, 11, 82,
                           &lv_font_montserrat_14, COLOR_GREEN);
  lv_label_set_long_mode(home_status, LV_LABEL_LONG_CLIP);
  lv_obj_set_style_text_align(home_status, LV_TEXT_ALIGN_LEFT, 0);
}

lv_obj_t *remaining_bar(lv_obj_t *parent, int x, int y, int width, int height) {
  lv_obj_t *bar = lv_bar_create(parent);
  lv_obj_set_pos(bar, x, y);
  lv_obj_set_size(bar, width, height);
  lv_bar_set_range(bar, 0, 100);
  lv_obj_remove_flag(bar, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_style_bg_color(bar, lv_color_hex(0x2b4658), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_radius(bar, height / 2, LV_PART_MAIN);
  lv_obj_set_style_bg_color(bar, lv_color_hex(COLOR_AMBER), LV_PART_INDICATOR);
  lv_obj_set_style_radius(bar, height / 2, LV_PART_INDICATOR);
  return bar;
}

void build_dashboard(lv_obj_t *overlay) {
  home_disconnected = false;
  build_home_header(overlay);

  const int left = 12;
  const int right = 234;
  const int width = 210;
  const int height = 84;
  const int rows[] = {68, 164, 260};

  const ServingVessel vessel = vessel_for_serving(latest_state.serving);
  lv_obj_t *card =
      metric_card(overlay, left, rows[0], width, height, "", COLOR_AMBER,
                  MetricGlyph::Serving, vessel);
  home_serving_label =
      make_label(card, "SERVINGS LEFT", 40, 9, width - 52,
                 &lv_font_montserrat_14, COLOR_MUTED);
  home_servings =
      make_label(card, "--", 14, 34, width - 28, &lv_font_montserrat_24);

  card = metric_card(overlay, right, rows[0], width, height, "REMAINING");
  home_percent =
      make_label(card, "--", 14, 34, width - 28, &lv_font_montserrat_24);

  card = metric_card(overlay, left, rows[1], width, height, "GALLONS LEFT",
                     COLOR_HEADER_ACCENT, MetricGlyph::Gallons);
  home_gallons =
      make_label(card, "--", 14, 34, width - 28, &lv_font_montserrat_20);

  card = metric_card(overlay, right, rows[1], width, height, "BEER WEIGHT",
                     0x91a9b6, MetricGlyph::BeerWeight);
  home_beer_weight =
      make_label(card, "--", 14, 34, width - 28, &lv_font_montserrat_20);

  card = metric_card(overlay, left, rows[2], width, height, "KEG SIZE",
                     0x91a9b6, MetricGlyph::Keg);
  home_capacity =
      make_label(card, "--", 14, 34, width - 28, &lv_font_montserrat_20);

  card = metric_card(overlay, right, rows[2], width, height, "SERVING SIZE",
                     COLOR_HEADER_ACCENT, MetricGlyph::ServingSize, vessel);
  home_serving_size =
      make_label(card, "--", 14, 34, width - 28, &lv_font_montserrat_20);
}

void build_minimal(lv_obj_t *overlay) {
  home_disconnected = false;
  build_home_header(overlay);

  // Minimal intentionally spends more of the screen on the three pieces of
  // information a person reads from across the room.
  home_servings =
      make_label(overlay, "--", 30, 64, 396, &lv_font_montserrat_48);
  lv_obj_set_style_text_align(home_servings, LV_TEXT_ALIGN_CENTER, 0);

  home_serving_label =
      make_label(overlay, "SERVINGS LEFT", 30, 122, 396,
                 &lv_font_montserrat_28, COLOR_TEXT);
  lv_obj_set_style_text_align(home_serving_label, LV_TEXT_ALIGN_CENTER, 0);

  home_serving_size =
      make_label(overlay, "--", 30, 162, 396,
                 &lv_font_montserrat_20, COLOR_MUTED);
  lv_obj_set_style_text_align(home_serving_size, LV_TEXT_ALIGN_CENTER, 0);

  lv_obj_t *remaining =
      metric_card(overlay, 12, 205, 432, 74, "REMAINING");
  home_percent =
      make_label(remaining, "--", 332, 11, 84, &lv_font_montserrat_24);
  lv_obj_set_style_text_align(home_percent, LV_TEXT_ALIGN_RIGHT, 0);
  home_progress = remaining_bar(remaining, 14, 49, 402, 12);

  lv_obj_t *card =
      metric_card(overlay, 12, 294, 210, 72, "GALLONS LEFT",
                  COLOR_HEADER_ACCENT, MetricGlyph::Gallons);
  home_gallons =
      make_label(card, "--", 14, 35, 182, &lv_font_montserrat_20);

  card = metric_card(overlay, 234, 294, 210, 72, "BEER WEIGHT",
                     0x91a9b6, MetricGlyph::BeerWeight);
  home_beer_weight =
      make_label(card, "--", 14, 35, 182, &lv_font_montserrat_20);
}

void build_gauge(lv_obj_t *overlay) {
  home_disconnected = false;
  build_home_header(overlay);

  home_arc = lv_arc_create(overlay);
  lv_obj_set_pos(home_arc, 8, 72);
  lv_obj_set_size(home_arc, 216, 216);
  lv_arc_set_rotation(home_arc, 270);
  lv_arc_set_bg_angles(home_arc, 0, 360);
  lv_arc_set_range(home_arc, 0, 100);
  lv_obj_remove_style(home_arc, nullptr, LV_PART_KNOB);
  lv_obj_remove_flag(home_arc, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_style_arc_width(home_arc, 19, LV_PART_MAIN);
  lv_obj_set_style_arc_width(home_arc, 19, LV_PART_INDICATOR);
  lv_obj_set_style_arc_color(home_arc, lv_color_hex(0x304d60), LV_PART_MAIN);
  lv_obj_set_style_arc_color(home_arc, lv_color_hex(COLOR_AMBER),
                             LV_PART_INDICATOR);

  home_percent = make_label(overlay, "--", 40, 119, 152,
                            &lv_font_montserrat_48);
  lv_obj_set_style_text_align(home_percent, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_t *remaining =
      make_label(overlay, "REMAINING", 48, 176, 136,
                 &lv_font_montserrat_14, COLOR_MUTED);
  lv_obj_set_style_text_align(remaining, LV_TEXT_ALIGN_CENTER, 0);
  home_servings = make_label(overlay, "--", 66, 204, 100,
                             &lv_font_montserrat_24);
  lv_obj_set_style_text_align(home_servings, LV_TEXT_ALIGN_CENTER, 0);
  home_serving_label =
      make_label(overlay, "SERVINGS LEFT", 42, 234, 148,
                 &lv_font_montserrat_14, COLOR_TEXT);
  lv_obj_set_style_text_align(home_serving_label, LV_TEXT_ALIGN_CENTER, 0);

  const ServingVessel vessel = vessel_for_serving(latest_state.serving);
  const int card_x = 232;
  const int card_w = 212;
  const int card_h = 62;
  lv_obj_t *card =
      metric_card(overlay, card_x, 68, card_w, card_h, "GALLONS LEFT",
                  COLOR_HEADER_ACCENT, MetricGlyph::Gallons);
  home_gallons =
      make_label(card, "--", 14, 31, 184, &lv_font_montserrat_20);

  card = metric_card(overlay, card_x, 140, card_w, card_h, "BEER WEIGHT",
                     0x91a9b6, MetricGlyph::BeerWeight);
  home_beer_weight =
      make_label(card, "--", 14, 31, 184, &lv_font_montserrat_20);

  card = metric_card(overlay, card_x, 212, card_w, card_h, "SERVING SIZE",
                     COLOR_HEADER_ACCENT, MetricGlyph::ServingSize, vessel);
  home_serving_size =
      make_label(card, "--", 14, 31, 184, &lv_font_montserrat_20);

  card = metric_card(overlay, card_x, 284, card_w, card_h, "KEG SIZE",
                     0x91a9b6, MetricGlyph::Keg);
  home_capacity =
      make_label(card, "--", 14, 31, 184, &lv_font_montserrat_20);
}

void build_keg_level(lv_obj_t *overlay) {
  home_disconnected = false;
  build_home_header(overlay);

  lv_obj_t *keg_body = lv_obj_create(overlay);
  lv_obj_set_pos(keg_body, 20, 84);
  lv_obj_set_size(keg_body, 188, 266);
  lv_obj_remove_flag(keg_body, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_remove_flag(keg_body, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_style_bg_color(keg_body, lv_color_hex(0x14232d), 0);
  lv_obj_set_style_bg_opa(keg_body, LV_OPA_COVER, 0);
  lv_obj_set_style_border_color(keg_body, lv_color_hex(COLOR_GLASS), 0);
  lv_obj_set_style_border_width(keg_body, 4, 0);
  lv_obj_set_style_radius(keg_body, 28, 0);
  lv_obj_set_style_pad_all(keg_body, 0, 0);

  outline_shape(overlay, 61, 70, 106, 16, COLOR_GLASS, 3, 8);
  outline_shape(overlay, 97, 61, 34, 10, COLOR_GLASS, 3, 5);

  // Use a rounded, clipped inner barrel and ONE continuous liquid object.
  // This removes the segmented rectangular bands that never matched the keg
  // walls on hardware. The parent clips the liquid to the same rounded barrel
  // geometry, so the fill reaches the inner wall and curved base cleanly.
  lv_obj_t *keg_inner = lv_obj_create(keg_body);
  // Child coordinates are already relative to the keg body's content area,
  // which starts inside the 4 px outer border. The previous 4 px offset
  // double-inset the liquid on the left/top and is visible on hardware.
  lv_obj_set_pos(keg_inner, 0, 0);
  lv_obj_set_size(keg_inner, KEG_INNER_WIDTH, KEG_INNER_HEIGHT);
  lv_obj_remove_flag(keg_inner, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_remove_flag(keg_inner, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_style_bg_opa(keg_inner, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(keg_inner, 0, 0);
  lv_obj_set_style_radius(keg_inner, 24, 0);
  lv_obj_set_style_clip_corner(keg_inner, true, 0);
  lv_obj_set_style_pad_all(keg_inner, 0, 0);

  keg_fill = lv_obj_create(keg_inner);
  lv_obj_set_pos(keg_fill, 0, KEG_INNER_HEIGHT);
  lv_obj_set_size(keg_fill, KEG_INNER_WIDTH, 1);
  lv_obj_remove_flag(keg_fill, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_remove_flag(keg_fill, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_style_bg_color(keg_fill, lv_color_hex(COLOR_AMBER), 0);
  lv_obj_set_style_bg_opa(keg_fill, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(keg_fill, 0, 0);
  lv_obj_set_style_radius(keg_fill, 0, 0);
  lv_obj_set_style_pad_all(keg_fill, 0, 0);
  lv_obj_add_flag(keg_fill, LV_OBJ_FLAG_HIDDEN);

  // Barrel chimes/ribs sit above the liquid. Keep them thin enough that the
  // liquid still reads as one continuous volume.
  shape(keg_body, 10, 16, 160, 4, 0x78909c, 2);
  shape(keg_body, 8, 68, 164, 3, 0x5f7986, 2);
  shape(keg_body, 8, 188, 164, 3, 0x5f7986, 2);
  shape(keg_body, 10, 239, 160, 4, 0x78909c, 2);

  home_percent =
      make_label(overlay, "--", 54, 165, 120, &lv_font_montserrat_28,
                 COLOR_TEXT);
  lv_obj_set_style_text_align(home_percent, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_t *remaining =
      make_label(overlay, "REMAINING", 54, 200, 120,
                 &lv_font_montserrat_14, COLOR_MUTED);
  lv_obj_set_style_text_align(remaining, LV_TEXT_ALIGN_CENTER, 0);

  const ServingVessel vessel = vessel_for_serving(latest_state.serving);
  const int x = 220;
  const int w = 224;
  const int h = 62;
  lv_obj_t *card =
      metric_card(overlay, x, 66, w, h, "", COLOR_AMBER,
                  MetricGlyph::Serving, vessel);
  home_serving_label =
      make_label(card, "SERVINGS LEFT", 40, 8, w - 52,
                 &lv_font_montserrat_14, COLOR_MUTED);
  home_servings =
      make_label(card, "--", 14, 31, w - 28, &lv_font_montserrat_20);

  card = metric_card(overlay, x, 138, w, h, "GALLONS LEFT",
                     COLOR_HEADER_ACCENT, MetricGlyph::Gallons);
  home_gallons =
      make_label(card, "--", 14, 31, w - 28, &lv_font_montserrat_20);

  card = metric_card(overlay, x, 210, w, h, "BEER WEIGHT", 0x91a9b6,
                     MetricGlyph::BeerWeight);
  home_beer_weight =
      make_label(card, "--", 14, 31, w - 28, &lv_font_montserrat_20);

  card = metric_card(overlay, x, 282, w, h, "SERVING SIZE",
                     COLOR_HEADER_ACCENT, MetricGlyph::ServingSize, vessel);
  home_serving_size =
      make_label(card, "--", 14, 31, w - 28, &lv_font_montserrat_20);
}

lv_obj_t *service_row(lv_obj_t *overlay, int y, const char *title,
                      MetricGlyph glyph = MetricGlyph::None,
                      ServingVessel vessel = ServingVessel::Generic,
                      uint32_t color = COLOR_HEADER_ACCENT) {
  lv_obj_t *row = lv_obj_create(overlay);
  lv_obj_set_pos(row, 8, y);
  lv_obj_set_size(row, 436, 28);
  lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_style_bg_color(row, lv_color_hex(0x152733), 0);
  lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
  lv_obj_set_style_border_color(row, lv_color_hex(0x315369), 0);
  lv_obj_set_style_border_width(row, 1, 0);
  lv_obj_set_style_radius(row, 7, 0);
  lv_obj_set_style_pad_all(row, 0, 0);

  const int text_x = glyph == MetricGlyph::None ? 10 : 36;
  if (glyph != MetricGlyph::None)
    draw_metric_icon(row, glyph, 10, 4, color, vessel);
  make_label(row, title, text_x, 5, 258 - text_x,
             &lv_font_montserrat_14, COLOR_MUTED);
  lv_obj_t *value =
      make_label(row, "--", 262, 5, 162, &lv_font_montserrat_14, COLOR_TEXT);
  lv_obj_set_style_text_align(value, LV_TEXT_ALIGN_RIGHT, 0);
  return value;
}

void build_service(lv_obj_t *overlay) {
  home_disconnected = false;
  build_home_header(overlay);

  const ServingVessel vessel = vessel_for_serving(latest_state.serving);
  const char *titles[11] = {
      "SERVINGS LEFT", "REMAINING", "GALLONS LEFT", "BEER WEIGHT",
      "SCALE WEIGHT", "EMPTY KEG / TARE", "KEG CAPACITY", "SERVING SIZE",
      "SCALE FIRMWARE", "TOUCH FIRMWARE", "CALIBRATION"};
  const MetricGlyph glyphs[11] = {
      MetricGlyph::Serving, MetricGlyph::None, MetricGlyph::Gallons,
      MetricGlyph::BeerWeight, MetricGlyph::ScaleWeight, MetricGlyph::EmptyKeg,
      MetricGlyph::Keg, MetricGlyph::ServingSize, MetricGlyph::Firmware,
      MetricGlyph::Display, MetricGlyph::Calibration};
  const uint32_t colors[11] = {
      COLOR_AMBER, COLOR_HEADER_ACCENT, COLOR_HEADER_ACCENT, 0x91a9b6,
      0x91a9b6, 0x91a9b6, 0x91a9b6, COLOR_HEADER_ACCENT,
      COLOR_HEADER_ACCENT, COLOR_HEADER_ACCENT, COLOR_GREEN};

  for (int i = 0; i < 11; ++i)
    service_values[i] =
        service_row(overlay, 52 + i * 29, titles[i], glyphs[i], vessel,
                    colors[i]);
}

lv_obj_t *glass_metric_card(lv_obj_t *overlay, int y, const char *title,
                            uint32_t accent = COLOR_HEADER_ACCENT,
                            MetricGlyph glyph = MetricGlyph::None) {
  lv_obj_t *card = lv_obj_create(overlay);
  lv_obj_set_pos(card, 214, y);
  lv_obj_set_size(card, 230, 82);
  lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_style_bg_color(card, lv_color_hex(COLOR_METRIC_CARD), 0);
  lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
  lv_obj_set_style_border_color(card, lv_color_hex(COLOR_METRIC_BORDER), 0);
  lv_obj_set_style_border_width(card, 1, 0);
  lv_obj_set_style_radius(card, 12, 0);
  lv_obj_set_style_pad_all(card, 0, 0);
  shape(card, 0, 15, 4, 52, accent, 2);
  const int title_x = glyph == MetricGlyph::None ? 14 : 40;
  if (glyph != MetricGlyph::None)
    draw_metric_icon(card, glyph, 14, 8, accent);
  make_label(card, title, title_x, 10, 216 - title_x,
             &lv_font_montserrat_14, COLOR_MUTED);
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
    lv_obj_set_pos(outline, crowler ? 31 : 39, 66);
    lv_obj_set_size(outline, crowler ? 156 : 140, 280);
    lv_obj_remove_flag(outline, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_opa(outline, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_color(outline, lv_color_hex(COLOR_GLASS), 0);
    lv_obj_set_style_border_width(outline, 5, 0);
    lv_obj_set_style_radius(outline, crowler ? 11 : 18, 0);

    lv_obj_t *seam = lv_obj_create(overlay);
    lv_obj_set_pos(seam, crowler ? 43 : 51, 79);
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
    lv_obj_set_pos(outline, 8, 58);
  } else if (vessel == ServingVessel::SoloCup) {
    lv_line_set_points(outline, solo_outline_points,
                       sizeof(solo_outline_points) /
                           sizeof(solo_outline_points[0]));
    lv_obj_set_pos(outline, 22, 66);
  } else {
    lv_line_set_points(outline, pint_outline_points,
                       sizeof(pint_outline_points) /
                           sizeof(pint_outline_points[0]));
    lv_obj_set_pos(outline, 22, 66);
  }

  lv_obj_set_style_line_color(outline, lv_color_hex(COLOR_GLASS), 0);
  lv_obj_set_style_line_width(outline, 6, 0);
  lv_obj_set_style_line_rounded(outline, true, 0);

  if (vessel == ServingVessel::SoloCup) {
    lv_obj_t *rim = lv_obj_create(overlay);
    lv_obj_set_pos(rim, 39, 76);
    lv_obj_set_size(rim, 140, 10);
    lv_obj_remove_flag(rim, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_opa(rim, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_color(rim, lv_color_hex(COLOR_GLASS), 0);
    lv_obj_set_style_border_width(rim, 5, 0);
    lv_obj_set_style_radius(rim, 8, 0);

    lv_obj_t *base = lv_obj_create(overlay);
    lv_obj_set_pos(base, 72, 330);
    lv_obj_set_size(base, 74, 3);
    lv_obj_remove_flag(base, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(base, lv_color_hex(COLOR_GLASS), 0);
    lv_obj_set_style_border_width(base, 0, 0);
  }
}

void build_glass(lv_obj_t *overlay) {
  home_disconnected = false;
  build_home_header(overlay);

  rendered_vessel = vessel_for_serving(latest_state.serving);

  for (int i = 0; i < GLASS_BAND_COUNT; ++i) {
    int x = 0, y = 0, width = 0, height = 0;
    vessel_band_geometry(rendered_vessel, i, &x, &y, &width, &height);
    y += 8;

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

  home_servings = make_label(overlay, "--", 34, 120, 150,
                             &lv_font_montserrat_48);
  lv_obj_set_style_text_align(home_servings, LV_TEXT_ALIGN_CENTER, 0);

  home_serving_label = make_label(overlay, "SERVINGS LEFT", 25, 174, 168,
                                  &lv_font_montserrat_16, COLOR_TEXT);
  lv_label_set_long_mode(home_serving_label, LV_LABEL_LONG_WRAP);
  lv_obj_set_height(home_serving_label, 48);
  lv_obj_set_style_text_line_space(home_serving_label, -2, 0);
  lv_obj_set_style_text_align(home_serving_label, LV_TEXT_ALIGN_CENTER, 0);

  lv_obj_t *serving_size_badge = lv_obj_create(overlay);
  lv_obj_set_pos(serving_size_badge, 53, 212);
  lv_obj_set_size(serving_size_badge, 112, 30);
  lv_obj_remove_flag(serving_size_badge, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_style_bg_color(serving_size_badge, lv_color_hex(0x111820), 0);
  lv_obj_set_style_bg_opa(serving_size_badge, LV_OPA_COVER, 0);
  lv_obj_set_style_border_color(serving_size_badge, lv_color_hex(0x4c5961), 0);
  lv_obj_set_style_border_width(serving_size_badge, 1, 0);
  lv_obj_set_style_radius(serving_size_badge, 15, 0);
  lv_obj_set_style_pad_all(serving_size_badge, 0, 0);

  home_serving_size =
      make_label(serving_size_badge, "-- OZ EACH", 0, 6, 112,
                 &lv_font_montserrat_14, COLOR_TEXT);
  lv_obj_set_style_text_align(home_serving_size, LV_TEXT_ALIGN_CENTER, 0);

  lv_obj_t *card =
      glass_metric_card(overlay, 72, "REMAINING");
  home_percent = make_label(card, "--", 14, 34, 202,
                            &lv_font_montserrat_24);

  card = glass_metric_card(overlay, 170, "GALLONS LEFT",
                           COLOR_HEADER_ACCENT, MetricGlyph::Gallons);
  home_gallons = make_label(card, "--", 14, 36, 202,
                            &lv_font_montserrat_20);

  card = glass_metric_card(overlay, 268, "BEER WEIGHT", 0x91a9b6,
                           MetricGlyph::BeerWeight);
  home_beer_weight = make_label(card, "--", 14, 36, 202,
                                &lv_font_montserrat_20);
}

void update_home_values();

void build_selected_home(lv_obj_t *overlay) {
  rendered_vessel = vessel_for_serving(latest_state.serving);
  switch (home_view) {
  case HomeView::Glass:
    build_glass(overlay);
    break;
  case HomeView::Dashboard:
    build_dashboard(overlay);
    break;
  case HomeView::Minimal:
    build_minimal(overlay);
    break;
  case HomeView::Gauge:
    build_gauge(overlay);
    break;
  case HomeView::KegLevel:
    build_keg_level(overlay);
    break;
  case HomeView::Service:
    build_service(overlay);
    break;
  default:
    home_view = HomeView::Dashboard;
    build_dashboard(overlay);
    break;
  }
}

void rebuild_home_for_selected_view() {
  if (!home_overlay)
    return;
  lv_obj_clean(home_overlay);
  reset_home_child_refs();
  home_disconnected = false;
  if (!latest_state.online)
    update_disconnected(home_overlay);
  else
    build_selected_home(home_overlay);
}

void set_home_view(HomeView view) {
  touchscreen_ui_clear_notice_locked();
  home_view = view;
  save_preference(home_view);
  rebuild_home_for_selected_view();
  if (view_button_label)
    lv_label_set_text(view_button_label, home_view_name(home_view));
  update_home_values();
}

void next_home_view_event(lv_event_t *) {
  set_home_view(next_home_view(home_view));
}

void previous_home_view_event(lv_event_t *) {
  set_home_view(previous_home_view(home_view));
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

    if (y >= 428 && y <= 455 && x <= 20 && w >= 140 && h >= 34) {
      lv_obj_set_pos(child, 12, 432);
      lv_obj_set_size(child, 164, 40);
      lv_obj_set_style_bg_color(child, lv_color_hex(COLOR_HEADER_BUTTON), 0);
      lv_obj_set_style_border_color(child, lv_color_hex(COLOR_HEADER_ACCENT), 0);
      lv_obj_set_style_border_width(child, 1, 0);
      lv_obj_set_style_radius(child, 12, 0);
      break;
    }
  }
}

lv_obj_t *selector_arrow(lv_obj_t *parent, const char *glyph, int x,
                         lv_event_cb_t callback) {
  lv_obj_t *button = lv_button_create(parent);
  lv_obj_set_pos(button, x, 0);
  lv_obj_set_size(button, 30, 38);
  lv_obj_set_style_bg_opa(button, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(button, 0, 0);
  lv_obj_set_style_radius(button, 10, 0);
  lv_obj_set_style_bg_color(button, lv_color_hex(0x276990), LV_STATE_PRESSED);
  lv_obj_t *text = lv_label_create(button);
  lv_label_set_text(text, glyph);
  lv_obj_set_style_text_font(text, &lv_font_montserrat_20, 0);
  lv_obj_set_style_text_color(text, lv_color_hex(COLOR_TEXT), 0);
  lv_obj_center(text);
  lv_obj_add_event_cb(button, callback, LV_EVENT_CLICKED, nullptr);
  return button;
}

void ensure_view_button() {
  if (!view_button) {
    lv_obj_t *root = lv_screen_active();
    view_button = lv_obj_create(root);
    lv_obj_set_pos(view_button, 318, 432);
    lv_obj_set_size(view_button, 150, 40);
    lv_obj_remove_flag(view_button, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(view_button, lv_color_hex(COLOR_HEADER_BUTTON), 0);
    lv_obj_set_style_bg_opa(view_button, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(view_button, lv_color_hex(COLOR_HEADER_ACCENT), 0);
    lv_obj_set_style_border_width(view_button, 1, 0);
    lv_obj_set_style_radius(view_button, 12, 0);
    lv_obj_set_style_pad_all(view_button, 0, 0);

    view_prev_button =
        selector_arrow(view_button, "<", 0, previous_home_view_event);
    view_next_button =
        selector_arrow(view_button, ">", 118, next_home_view_event);

    view_button_label = lv_label_create(view_button);
    lv_obj_set_pos(view_button_label, 30, 11);
    lv_obj_set_width(view_button_label, 88);
    lv_obj_set_style_text_font(view_button_label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(view_button_label, lv_color_hex(COLOR_TEXT), 0);
    lv_obj_set_style_text_align(view_button_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(view_button_label, LV_LABEL_LONG_DOT);

    lv_obj_add_event_cb(view_button, clear_view_button_refs, LV_EVENT_DELETE,
                        nullptr);
  }

  if (active_page == 0 && !home_menu_open && !home_keyboard_open &&
      !touchscreen_pairing_overlay_visible()) {
    tune_home_footer_layout();
    lv_label_set_text(view_button_label, home_view_name(home_view));
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
  else
    build_selected_home(home_overlay);

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

  if (home_disconnected || !home_name)
    rebuild_home_for_selected_view();

  if (vessel_for_serving(latest_state.serving) != rendered_vessel)
    rebuild_home_for_selected_view();

  char name[48];
  ascii_safe(latest_state.name[0] ? latest_state.name : "Keg Scale", name,
             sizeof(name));
  if (home_name)
    lv_label_set_text(home_name, name);

  const char *status = latest_state.stable ? "Stable" : "Settling";
  if (home_status) {
    lv_label_set_text(home_status, status);
    lv_obj_set_style_text_color(
        home_status,
        lv_color_hex(latest_state.stable ? COLOR_GREEN : COLOR_WARNING), 0);
  }
  if (home_status_dot) {
    lv_obj_set_style_bg_color(
        home_status_dot,
        lv_color_hex(latest_state.stable ? COLOR_GREEN : COLOR_WARNING), 0);
  }

  const char *serving_label = serving_count_label(latest_state.serving);
  if (home_serving_label)
    lv_label_set_text(home_serving_label, serving_label);

  char buffer[96];
  format_serving_size(latest_state.serving, buffer, sizeof(buffer));
  if (home_serving_size)
    lv_label_set_text(home_serving_size, buffer);

  if (home_capacity) {
    snprintf(buffer, sizeof(buffer), "%.2f gal", (double)latest_state.capacity);
    lv_label_set_text(home_capacity, buffer);
  }

  // Service view intentionally uses only values currently provided by the
  // scale. Temperature/history metrics are not synthesized on the touchscreen.
  if (service_values[4]) {
    snprintf(buffer, sizeof(buffer), "%.1f lb", (double)latest_state.weight);
    lv_label_set_text(service_values[4], buffer);
  }
  if (service_values[5]) {
    snprintf(buffer, sizeof(buffer), "%.1f lb", (double)latest_state.empty);
    lv_label_set_text(service_values[5], buffer);
  }
  if (service_values[6]) {
    snprintf(buffer, sizeof(buffer), "%.2f gal", (double)latest_state.capacity);
    lv_label_set_text(service_values[6], buffer);
  }
  if (service_values[7]) {
    format_serving_size(latest_state.serving, buffer, sizeof(buffer));
    lv_label_set_text(service_values[7], buffer);
  }
  if (service_values[8])
    lv_label_set_text(service_values[8],
                      latest_state.firmware[0] ? latest_state.firmware : "--");
  if (service_values[9]) {
    const esp_app_desc_t *app = esp_app_get_description();
    lv_label_set_text(service_values[9],
                      app && app->version[0] ? app->version : "--");
  }
  if (service_values[10])
    lv_label_set_text(service_values[10],
                      latest_state.calibrated ? "Calibrated" : "Not calibrated");

  if (!latest_state.ready) {
    if (home_percent)
      lv_label_set_text(home_percent, "--");
    if (home_servings)
      lv_label_set_text(home_servings, "--");
    if (home_gallons)
      lv_label_set_text(home_gallons, "--");
    if (home_beer_weight)
      lv_label_set_text(home_beer_weight, "--");
    if (home_scale_weight)
      lv_label_set_text(home_scale_weight, "--");
    if (home_tare)
      lv_label_set_text(home_tare, "");
    for (int i = 0; i < 4; ++i) {
      if (service_values[i])
        lv_label_set_text(service_values[i], "--");
    }
    if (home_arc)
      lv_arc_set_value(home_arc, 0);
    if (home_progress)
      lv_bar_set_value(home_progress, 0, LV_ANIM_OFF);
    for (auto *band : glass_bands) {
      if (band)
        lv_obj_add_flag(band, LV_OBJ_FLAG_HIDDEN);
    }
    if (keg_fill)
      lv_obj_add_flag(keg_fill, LV_OBJ_FLAG_HIDDEN);
    return;
  }

  const int percent = std::clamp(
      static_cast<int>(std::lround(latest_state.percent)), 0, 100);
  const uint32_t level_color = remaining_color(percent);
  const int servings =
      std::max(0, static_cast<int>(std::floor(latest_state.servings)));

  snprintf(buffer, sizeof(buffer), "%d%%", percent);
  if (home_percent)
    lv_label_set_text(home_percent, buffer);
  if (service_values[1])
    lv_label_set_text(service_values[1], buffer);

  snprintf(buffer, sizeof(buffer), "%d", servings);
  if (home_servings)
    lv_label_set_text(home_servings, buffer);
  if (service_values[0])
    lv_label_set_text(service_values[0], buffer);

  snprintf(buffer, sizeof(buffer), "%.2f gal", (double)latest_state.gallons);
  if (home_gallons)
    lv_label_set_text(home_gallons, buffer);
  if (service_values[2])
    lv_label_set_text(service_values[2], buffer);

  snprintf(buffer, sizeof(buffer), "%.1f lb", (double)beer_weight(latest_state));
  if (home_beer_weight)
    lv_label_set_text(home_beer_weight, buffer);
  if (service_values[3])
    lv_label_set_text(service_values[3], buffer);

  if (home_scale_weight) {
    snprintf(buffer, sizeof(buffer), "%.1f lb", (double)latest_state.weight);
    lv_label_set_text(home_scale_weight, buffer);
  }

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

  if (home_progress) {
    lv_bar_set_value(home_progress, percent, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(home_progress, lv_color_hex(level_color),
                              LV_PART_INDICATOR);
  }

  if (home_view == HomeView::Glass) {
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
  }

  if (home_view == HomeView::KegLevel && keg_fill) {
    lv_obj_set_style_bg_color(keg_fill, lv_color_hex(level_color), 0);
    if (percent <= 0) {
      lv_obj_add_flag(keg_fill, LV_OBJ_FLAG_HIDDEN);
    } else {
      const int fill_height =
          std::max(1, (percent * KEG_INNER_HEIGHT + 99) / 100);
      lv_obj_set_pos(keg_fill, 0, KEG_INNER_HEIGHT - fill_height);
      lv_obj_set_size(keg_fill, KEG_INNER_WIDTH, fill_height);
      lv_obj_remove_flag(keg_fill, LV_OBJ_FLAG_HIDDEN);
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
