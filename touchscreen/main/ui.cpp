#include "app.h"
#include "ui_lifetime.h"
#include "esp_lcd_touch.h"
#include "bsp/esp32_s3_touch_lcd_4b.h"
#include "bsp/touch.h"
#include "esp_log.h"
#include "cJSON.h"
#include "esp_app_desc.h"
#include "lvgl.h"
#include "lwip/inet.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

// Implemented by home_layout.cpp. Home is rendered synchronously by the
// custom Dashboard/Glass renderer instead of the legacy Home widgets.
void touchscreen_home_render_now();
void touchscreen_home_set_menu_open(bool open);
void touchscreen_home_set_keyboard_open(bool open);
// Implemented by ui_wrapper.cpp. Render the custom OTA page synchronously so
// the legacy Firmware & diagnostics page never flashes first.
void touchscreen_update_page_render_now();
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
lv_obj_t *capacity_dropdown = nullptr;
lv_obj_t *home_nav_button = nullptr;
lv_obj_t *menu_handle_button = nullptr;
lv_obj_t *menu_scrim = nullptr;
lv_obj_t *menu_panel = nullptr;
lv_obj_t *qr_fullscreen = nullptr;
lv_obj_t *connection_badge = nullptr;
// TOUCH_DRAWER_V1
// TOUCH_SHELL_POLISH_V3
// TOUCH_LAYOUT_POLISH_V4
// TOUCH_DRAWER_REFINEMENTS_V2_4
char discovered_scale_options[800] = "Manual IP / hostname...";
TouchscreenUiGeneration content_generation{1};
bool ui_initialized = false;
const uint32_t BG = 0x101c26, CARD = 0x203441, ACCENT = 0x54d6bf,
               TEXT = 0xf2f6f8;
// TOUCH_BUTTON_STYLE_V5
void build(int page);
void focus(lv_event_t *e);
void message(const char *s) { lv_label_set_text(notice, s); }
bool submit(const char *kind, cJSON *json) {
  Action a{};
  a.ui_generation = touchscreen_ui_generation_current(&content_generation);
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

  // Shared modern action-button treatment used throughout all sub-pages.
  // Individual controls such as the drawer close button may further override
  // these defaults after creation.
  lv_obj_set_style_bg_color(o, lv_color_hex(0x2a3945), 0);
  lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
  lv_obj_set_style_border_color(o, lv_color_hex(0x587181), 0);
  lv_obj_set_style_border_width(o, 1, 0);
  lv_obj_set_style_radius(o, 15, 0);

  // Give taps an obvious but restrained pressed-state response.
  lv_obj_set_style_bg_color(o, lv_color_hex(0x36505f), LV_STATE_PRESSED);
  lv_obj_set_style_border_color(o, lv_color_hex(ACCENT), LV_STATE_PRESSED);

  auto t = lv_label_create(o);
  lv_label_set_text(t, s);
  lv_obj_set_style_text_color(t, lv_color_hex(TEXT), 0);
  lv_obj_center(t);
  lv_obj_add_event_cb(o, cb, LV_EVENT_CLICKED, data);
  return o;
}

void set_menu_x(void *obj, int32_t x) {
  lv_obj_set_x(static_cast<lv_obj_t *>(obj), x);
}

void animate_menu_x(int32_t from, int32_t to) {
  if (!menu_panel)
    return;
  lv_anim_delete(menu_panel, set_menu_x);
  lv_anim_t anim;
  lv_anim_init(&anim);
  lv_anim_set_var(&anim, menu_panel);
  lv_anim_set_values(&anim, from, to);
  lv_anim_set_duration(&anim, 220);
  lv_anim_set_exec_cb(&anim, set_menu_x);
  lv_anim_set_path_cb(&anim, lv_anim_path_ease_out);
  lv_anim_start(&anim);
}

void active_scale_web_url(char *url, size_t url_size,
                          char *host_text, size_t host_text_size) {
  if (url && url_size)
    url[0] = 0;
  if (host_text && host_text_size)
    host_text[0] = 0;

  const char *host = scale_hosts_ui[active_scale_ui];
  if (!host || !host[0])
    return;

  if (host_text && host_text_size)
    snprintf(host_text, host_text_size, "%s", host);

  if (!url || !url_size)
    return;

  if (strstr(host, "://"))
    snprintf(url, url_size, "%s", host);
  else
    snprintf(url, url_size, "http://%s/", host);
}


bool active_scale_ip_url(char *url, size_t url_size,
                         char *ip_text, size_t ip_text_size) {
  if (url && url_size)
    url[0] = 0;
  if (ip_text && ip_text_size)
    ip_text[0] = 0;

  const char *saved = scale_hosts_ui[active_scale_ui];
  if (!saved || !saved[0])
    return false;

  char lookup[128] = {};
  const char *start = saved;
  if (!strncmp(start, "http://", 7))
    start += 7;
  else if (!strncmp(start, "https://", 8))
    start += 8;

  size_t n = 0;
  while (start[n] && start[n] != '/' && start[n] != ':' &&
         n + 1 < sizeof(lookup)) {
    lookup[n] = start[n];
    ++n;
  }
  lookup[n] = 0;
  if (!lookup[0])
    return false;

  struct addrinfo hints = {};
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_STREAM;
  struct addrinfo *result = nullptr;

  const int rc = getaddrinfo(lookup, nullptr, &hints, &result);
  if (rc != 0 || !result)
    return false;

  bool ok = false;
  if (result->ai_addr && result->ai_family == AF_INET) {
    const auto *addr =
        reinterpret_cast<const struct sockaddr_in *>(result->ai_addr);
    char resolved[INET_ADDRSTRLEN] = {};
    if (inet_ntop(AF_INET, &addr->sin_addr, resolved, sizeof(resolved))) {
      if (ip_text && ip_text_size)
        snprintf(ip_text, ip_text_size, "%s", resolved);
      if (url && url_size)
        snprintf(url, url_size, "http://%s/", resolved);
      ok = true;
    }
  }

  freeaddrinfo(result);
  return ok;
}

void close_qr_fullscreen(lv_event_t *) {
  if (!qr_fullscreen)
    return;
  lv_obj_delete(qr_fullscreen);
  qr_fullscreen = nullptr;
}

void show_qr_fullscreen(lv_event_t *) {
  char name_url[192];
  char host[128];
  active_scale_web_url(name_url, sizeof(name_url), host, sizeof(host));
  if (!name_url[0])
    return;

  char ip_url[64] = {};
  char ip_text[32] = {};
  const bool have_ip =
      active_scale_ip_url(ip_url, sizeof(ip_url), ip_text, sizeof(ip_text));

  if (qr_fullscreen) {
    lv_obj_delete(qr_fullscreen);
    qr_fullscreen = nullptr;
  }

  lv_obj_t *root = lv_screen_active();
  qr_fullscreen = lv_obj_create(root);
  lv_obj_set_pos(qr_fullscreen, 0, 0);
  lv_obj_set_size(qr_fullscreen, 480, 480);
  lv_obj_remove_flag(qr_fullscreen, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_style_bg_color(qr_fullscreen, lv_color_hex(BG), 0);
  lv_obj_set_style_bg_opa(qr_fullscreen, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(qr_fullscreen, 0, 0);
  lv_obj_set_style_radius(qr_fullscreen, 0, 0);
  lv_obj_set_style_pad_all(qr_fullscreen, 0, 0);

  auto title = label(qr_fullscreen, "Manage this scale", 30, 18, 420,
                     &lv_font_montserrat_24);
  lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, 0);

  auto help = label(qr_fullscreen, "Choose the address that works best",
                    30, 50, 420, &lv_font_montserrat_14);
  lv_obj_set_style_text_align(help, LV_TEXT_ALIGN_CENTER, 0);

  auto name_title = label(qr_fullscreen, "By Name", 34, 79, 180,
                          &lv_font_montserrat_18);
  lv_obj_set_style_text_align(name_title, LV_TEXT_ALIGN_CENTER, 0);

  lv_obj_t *name_qr = lv_qrcode_create(qr_fullscreen);
  lv_qrcode_set_size(name_qr, 154);
  lv_qrcode_set_dark_color(name_qr, lv_color_black());
  lv_qrcode_set_light_color(name_qr, lv_color_white());
  lv_qrcode_update(name_qr, name_url, strlen(name_url));
  lv_obj_set_pos(name_qr, 47, 108);
  lv_obj_set_style_border_color(name_qr, lv_color_white(), 0);
  lv_obj_set_style_border_width(name_qr, 5, 0);

  auto host_label = label(qr_fullscreen, host, 28, 275, 192,
                          &lv_font_montserrat_14);
  lv_label_set_long_mode(host_label, LV_LABEL_LONG_DOT);
  lv_obj_set_style_text_align(host_label, LV_TEXT_ALIGN_CENTER, 0);

  auto ip_title = label(qr_fullscreen, "By IP", 266, 79, 180,
                        &lv_font_montserrat_18);
  lv_obj_set_style_text_align(ip_title, LV_TEXT_ALIGN_CENTER, 0);

  if (have_ip) {
    lv_obj_t *ip_qr = lv_qrcode_create(qr_fullscreen);
    lv_qrcode_set_size(ip_qr, 154);
    lv_qrcode_set_dark_color(ip_qr, lv_color_black());
    lv_qrcode_set_light_color(ip_qr, lv_color_white());
    lv_qrcode_update(ip_qr, ip_url, strlen(ip_url));
    lv_obj_set_pos(ip_qr, 279, 108);
    lv_obj_set_style_border_color(ip_qr, lv_color_white(), 0);
    lv_obj_set_style_border_width(ip_qr, 5, 0);

    auto ip_label = label(qr_fullscreen, ip_text, 260, 275, 192,
                          &lv_font_montserrat_14);
    lv_obj_set_style_text_align(ip_label, LV_TEXT_ALIGN_CENTER, 0);
  } else {
    lv_obj_t *placeholder = lv_obj_create(qr_fullscreen);
    lv_obj_set_pos(placeholder, 279, 108);
    lv_obj_set_size(placeholder, 154, 154);
    lv_obj_remove_flag(placeholder, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(placeholder, lv_color_hex(0x202d36), 0);
    lv_obj_set_style_border_color(placeholder, lv_color_hex(0x52636e), 0);
    lv_obj_set_style_border_width(placeholder, 1, 0);
    lv_obj_set_style_radius(placeholder, 12, 0);

    auto unavailable = label(placeholder, "IP unavailable", 8, 58, 138,
                             &lv_font_montserrat_16);
    lv_obj_set_style_text_align(unavailable, LV_TEXT_ALIGN_CENTER, 0);

    auto ip_help = label(qr_fullscreen,
                         "Could not resolve the scale's current IP",
                         260, 275, 192, &lv_font_montserrat_14);
    lv_obj_set_style_text_align(ip_help, LV_TEXT_ALIGN_CENTER, 0);
  }

  auto note = label(qr_fullscreen,
                    "Name is easier to remember. IP bypasses name resolution.",
                    30, 322, 420, &lv_font_montserrat_14);
  lv_obj_set_style_text_align(note, LV_TEXT_ALIGN_CENTER, 0);

  auto back = button(qr_fullscreen, "Back", 166, 420, 148, close_qr_fullscreen);
  lv_obj_set_size(back, 148, 40);
  lv_obj_set_style_bg_color(back, lv_color_hex(0x2a3945), 0);
  lv_obj_set_style_border_color(back, lv_color_hex(0x587181), 0);
  lv_obj_set_style_border_width(back, 1, 0);
  lv_obj_set_style_radius(back, 20, 0);
  if (lv_obj_t *back_text = lv_obj_get_child(back, 0))
    lv_obj_set_style_text_color(back_text, lv_color_hex(TEXT), 0);
  lv_obj_set_ext_click_area(back, 6);

  lv_obj_move_foreground(qr_fullscreen);
}

void close_scale_menu(lv_event_t *) {
  touchscreen_home_set_menu_open(false);
  if (connection_badge)
    lv_obj_remove_flag(connection_badge, LV_OBJ_FLAG_HIDDEN);
  if (menu_scrim)
    lv_obj_add_flag(menu_scrim, LV_OBJ_FLAG_HIDDEN);
  if (menu_panel) {
    const int32_t current_x = lv_obj_get_x(menu_panel);
    animate_menu_x(current_x, 480);
  }
}

void drawer_nav(lv_event_t *event) {
  const int target =
      static_cast<int>(reinterpret_cast<intptr_t>(lv_event_get_user_data(event)));
  close_scale_menu(nullptr);
  build(target);
}

void populate_scale_menu() {
  if (!menu_panel)
    return;

  lv_obj_clean(menu_panel);

  label(menu_panel, "Scale Menu", 14, 14, 150, &lv_font_montserrat_20);
  auto close = button(menu_panel, "X", 182, 10, 32, close_scale_menu);
  lv_obj_set_size(close, 32, 32);
  lv_obj_set_style_bg_color(close, lv_color_hex(0x2a3945), 0);
  lv_obj_set_style_radius(close, 16, 0);
  lv_obj_set_ext_click_area(close, 10);
  if (lv_obj_t *close_text = lv_obj_get_child(close, 0))
    lv_obj_set_style_text_color(close_text, lv_color_hex(TEXT), 0);

  char url[192];
  char host[128];
  active_scale_web_url(url, sizeof(url), host, sizeof(host));

  if (url[0]) {
    lv_obj_t *qr = lv_qrcode_create(menu_panel);
    lv_qrcode_set_size(qr, 96);
    lv_qrcode_set_dark_color(qr, lv_color_black());
    lv_qrcode_set_light_color(qr, lv_color_white());
    lv_qrcode_update(qr, url, strlen(url));
    lv_obj_set_pos(qr, 66, 52);
    lv_obj_set_style_border_color(qr, lv_color_white(), 0);
    lv_obj_set_style_border_width(qr, 4, 0);
    lv_obj_add_flag(qr, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(qr, show_qr_fullscreen, LV_EVENT_CLICKED, nullptr);

    auto qr_help = label(menu_panel, "Scan to manage this scale", 14, 168, 200,
                         &lv_font_montserrat_14);
    lv_obj_set_style_text_align(qr_help, LV_TEXT_ALIGN_CENTER, 0);

    auto host_label =
        label(menu_panel, host, 14, 191, 200, &lv_font_montserrat_14);
    lv_label_set_long_mode(host_label, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(host_label, LV_TEXT_ALIGN_CENTER, 0);
  } else {
    auto no_scale =
        label(menu_panel, "Connect a scale to enable the QR code", 14, 84, 200,
              &lv_font_montserrat_16);
    lv_obj_set_style_text_align(no_scale, LV_TEXT_ALIGN_CENTER, 0);
  }

  auto drawer_button = [&](const char *title, int y, int page) {
    lv_obj_t *b = button(
        menu_panel, title, 14, y, 200, drawer_nav,
        reinterpret_cast<void *>(static_cast<intptr_t>(page)));
    lv_obj_set_size(b, 200, 42);
    lv_obj_set_style_bg_color(b, lv_color_hex(0x2a3945), 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(b, lv_color_hex(0x587181), 0);
    lv_obj_set_style_border_width(b, 1, 0);
    lv_obj_set_style_radius(b, 15, 0);
    if (lv_obj_t *t = lv_obj_get_child(b, 0))
      lv_obj_set_style_text_color(t, lv_color_hex(TEXT), 0);
    return b;
  };

  const int first_y = 229;
  const int gap = 47;
  drawer_button("Keg", first_y + gap * 0, 1);
  drawer_button("Scale", first_y + gap * 1, 2);
  drawer_button("Setup", first_y + gap * 2, 3);
  drawer_button("Update / Diagnostics", first_y + gap * 3, 4);
}

void open_scale_menu(lv_event_t *) {
  if (!menu_panel || !menu_scrim)
    return;

  populate_scale_menu();
  touchscreen_home_set_menu_open(true);
  if (connection_badge)
    lv_obj_add_flag(connection_badge, LV_OBJ_FLAG_HIDDEN);

  lv_obj_remove_flag(menu_scrim, LV_OBJ_FLAG_HIDDEN);
  lv_obj_move_foreground(menu_scrim);
  lv_obj_move_foreground(menu_panel);

  lv_obj_set_x(menu_panel, 480);
  animate_menu_x(480, 252);
}

void ensure_scale_menu(lv_obj_t *root) {
  if (!menu_scrim) {
    menu_scrim = lv_obj_create(root);
    lv_obj_set_pos(menu_scrim, 0, 0);
    lv_obj_set_size(menu_scrim, 480, 480);
    lv_obj_remove_flag(menu_scrim, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(menu_scrim, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_color(menu_scrim, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(menu_scrim, LV_OPA_50, 0);
    lv_obj_set_style_border_width(menu_scrim, 0, 0);
    lv_obj_set_style_radius(menu_scrim, 0, 0);
    lv_obj_set_style_pad_all(menu_scrim, 0, 0);
    lv_obj_add_event_cb(menu_scrim, close_scale_menu, LV_EVENT_CLICKED, nullptr);
    lv_obj_add_flag(menu_scrim, LV_OBJ_FLAG_HIDDEN);
  }

  if (!menu_panel) {
    menu_panel = lv_obj_create(root);
    lv_obj_set_pos(menu_panel, 480, 0);
    lv_obj_set_size(menu_panel, 228, 480);
    lv_obj_remove_flag(menu_panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(menu_panel, lv_color_hex(CARD), 0);
    lv_obj_set_style_bg_opa(menu_panel, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(menu_panel, lv_color_hex(ACCENT), 0);
    lv_obj_set_style_border_width(menu_panel, 0, 0);
    lv_obj_set_style_border_side(menu_panel, LV_BORDER_SIDE_LEFT, 0);
    lv_obj_set_style_radius(menu_panel, 0, 0);
    lv_obj_set_style_pad_all(menu_panel, 0, 0);
  }

  if (!menu_handle_button) {
    menu_handle_button = lv_button_create(root);
    lv_obj_set_pos(menu_handle_button, 426, 7);
    lv_obj_set_size(menu_handle_button, 42, 42);
    lv_obj_set_style_bg_color(menu_handle_button, lv_color_hex(0x2a3945), 0);
    lv_obj_set_style_bg_opa(menu_handle_button, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(menu_handle_button, lv_color_hex(0x587181), 0);
    lv_obj_set_style_border_width(menu_handle_button, 1, 0);
    lv_obj_set_style_radius(menu_handle_button, 21, 0);
    lv_obj_set_ext_click_area(menu_handle_button, 5);

    for (int i = 0; i < 3; ++i) {
      lv_obj_t *bar = lv_obj_create(menu_handle_button);
      lv_obj_remove_style_all(bar);
      lv_obj_set_size(bar, 18, 2);
      lv_obj_align(bar, LV_ALIGN_CENTER, 0, -6 + i * 6);
      lv_obj_remove_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
      lv_obj_remove_flag(bar, LV_OBJ_FLAG_CLICKABLE);
      lv_obj_set_style_bg_color(bar, lv_color_hex(TEXT), 0);
      lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
      lv_obj_set_style_radius(bar, 1, 0);
    }

    lv_obj_add_event_cb(menu_handle_button, open_scale_menu, LV_EVENT_CLICKED,
                        nullptr);
  }

  lv_obj_move_foreground(menu_handle_button);
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

  char caption[64];
  if (current.name[0]) {
    if (two_scales_ready())
      snprintf(caption, sizeof(caption), "%s  >", current.name);
    else
      snprintf(caption, sizeof(caption), "%s", current.name);
  } else {
    snprintf(caption, sizeof(caption), "%s",
             two_scales_ready() ? "Switch Scale  >" : "Home");
  }

  lv_label_set_text(text, caption);
  lv_label_set_long_mode(text, LV_LABEL_LONG_DOT);
  lv_obj_set_width(text, lv_obj_get_width(home_nav_button) - 18);
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
void toggle_password_visibility(lv_event_t *event) {
  auto button = static_cast<lv_obj_t *>(lv_event_get_target(event));
  auto password = static_cast<lv_obj_t *>(lv_event_get_user_data(event));
  if (!button || !password)
    return;

  const bool currently_hidden = lv_textarea_get_password_mode(password);
  lv_textarea_set_password_mode(password, !currently_hidden);

  if (lv_obj_t *button_text = lv_obj_get_child(button, 0))
    lv_label_set_text(button_text, currently_hidden ? "HIDE" : "SHOW");

  if (keyboard) {
    lv_keyboard_set_textarea(keyboard, password);
    lv_obj_move_foreground(keyboard);
  }
}

void dismiss_keyboard() {
  if (keyboard) {
    lv_keyboard_set_textarea(keyboard, nullptr);
    lv_obj_delete_async(keyboard);
    keyboard = nullptr;
    lv_obj_set_height(content, 356);
    touchscreen_home_set_keyboard_open(false);
  }
}
void keyboard_event(lv_event_t *) { dismiss_keyboard(); }
void focus(lv_event_t *e) {
  touchscreen_home_set_keyboard_open(true);

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
  lv_obj_move_foreground(keyboard);
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
uint16_t capacity_preset_index(float gallons) {
  if (fabsf(gallons - 15.5f) < 0.005f)
    return 1;
  if (fabsf(gallons - 7.75f) < 0.005f)
    return 2;
  if (fabsf(gallons - 5.17f) < 0.005f)
    return 3;
  if (fabsf(gallons - 5.0f) < 0.005f)
    return 4;
  return 0;
}

float capacity_preset_value(uint16_t index) {
  switch (index) {
  case 1:
    return 15.5f;
  case 2:
    return 7.75f;
  case 3:
    return 5.17f;
  case 4:
    return 5.0f;
  default:
    return 0.0f;
  }
}

void capacity_preset_changed(lv_event_t *event) {
  if (!fields[1])
    return;

  const uint16_t selected =
      lv_dropdown_get_selected((lv_obj_t *)lv_event_get_target(event));

  if (selected == 0) {
    lv_obj_remove_state(fields[1], LV_STATE_DISABLED);
    return;
  }

  char value[16];
  snprintf(value, sizeof(value), "%.2f",
           (double)capacity_preset_value(selected));
  lv_obj_remove_state(fields[1], LV_STATE_DISABLED);
  lv_textarea_set_text(fields[1], value);
  lv_obj_add_state(fields[1], LV_STATE_DISABLED);
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
  touchscreen_ui_generation_advance(&content_generation);
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
  touchscreen_ui_generation_advance(&content_generation);
  lv_obj_clean(content);
  lv_obj_scroll_to(content, 0, 0, LV_ANIM_OFF);
  headline = detail = connection = arc = networks = nullptr;
  scale_slot_dropdown = nullptr;
  scale_host_dropdown = nullptr;
  scale_manual_label = nullptr;
  scale_manual_host = nullptr;
  capacity_dropdown = nullptr;
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

    auto compact_field =
        [&](const char *title, const char *value, int x, int y, int width,
            bool numeric = false, int limit = 32) -> lv_obj_t * {
      label(content, title, x, y, width, &lv_font_montserrat_14);
      auto o = lv_textarea_create(content);
      lv_obj_set_pos(o, x, y + 18);
      lv_obj_set_size(o, width, 40);
      lv_textarea_set_one_line(o, true);
      lv_textarea_set_max_length(o, limit);
      lv_textarea_set_text(o, value);
      if (numeric)
        lv_textarea_set_accepted_chars(o, "0123456789.");
      lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLL_ON_FOCUS);
      lv_obj_add_event_cb(o, focus, LV_EVENT_SHORT_CLICKED, nullptr);
      return o;
    };

    fields[0] =
        compact_field("Beer / Beverage", current.name, 8, 34, 424, false);

    label(content, "Capacity (gal)", 8, 98, 207, &lv_font_montserrat_14);

    capacity_dropdown = lv_dropdown_create(content);
    lv_obj_set_pos(capacity_dropdown, 8, 116);
    lv_obj_set_size(capacity_dropdown, 128, 40);
    lv_dropdown_set_options(
        capacity_dropdown,
        "Custom\n1/2 barrel\n1/4 barrel\n1/6 barrel\nCorny keg");

    char capacity_value[16];
    snprintf(capacity_value, sizeof(capacity_value), "%.2f",
             (double)current.capacity);
    fields[1] = lv_textarea_create(content);
    lv_obj_set_pos(fields[1], 142, 116);
    lv_obj_set_size(fields[1], 73, 40);
    lv_textarea_set_one_line(fields[1], true);
    lv_textarea_set_max_length(fields[1], 8);
    lv_textarea_set_accepted_chars(fields[1], "0123456789.");
    lv_textarea_set_text(fields[1], capacity_value);
    lv_obj_remove_flag(fields[1], LV_OBJ_FLAG_SCROLL_ON_FOCUS);
    lv_obj_add_event_cb(fields[1], focus, LV_EVENT_SHORT_CLICKED, nullptr);

    const uint16_t capacity_selection =
        capacity_preset_index(current.capacity);
    lv_dropdown_set_selected(capacity_dropdown, capacity_selection);
    if (capacity_selection != 0)
      lv_obj_add_state(fields[1], LV_STATE_DISABLED);
    lv_obj_add_event_cb(capacity_dropdown, capacity_preset_changed,
                        LV_EVENT_VALUE_CHANGED, nullptr);

    char b[24];
    snprintf(b, sizeof(b), "%.2f", (double)current.empty);
    fields[2] =
        compact_field("Empty Keg (lb)", b, 225, 98, 207, true);

    snprintf(b, sizeof(b), "%.2f",
             (double)(current.density > 0 ? current.density : 8.34f));
    fields[3] =
        compact_field("Density (lb/gal)", b, 8, 160, 207, true);

    snprintf(b, sizeof(b), "%.2f",
             (double)(current.serving > 0 ? current.serving : 16));
    fields[4] =
        compact_field("Serving (oz)", b, 225, 160, 207, true);

    label(content, "Serving examples: 12 oz can • 16 oz pint • 64 oz growler",
          8, 224, 424, &lv_font_montserrat_14);
    button(content, "Save to scale", 8, 252, 207, save_keg);
    button(content, "Reload", 225, 252, 207, nav, (void *)1);
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
    label(content, "Setup", 8, 0, 190, &lv_font_montserrat_24);

    setup_scale_slot = active_scale_ui;

    auto compact_setup_field =
        [&](const char *title, const char *value, int x, int y, int width,
            bool password = false, int limit = 64) -> lv_obj_t * {
      label(content, title, x, y, width, &lv_font_montserrat_14);
      auto o = lv_textarea_create(content);
      lv_obj_set_pos(o, x, y + 17);
      lv_obj_set_size(o, width, 40);
      lv_textarea_set_one_line(o, true);
      lv_textarea_set_max_length(o, limit);
      lv_textarea_set_text(o, value);
      if (password)
        lv_textarea_set_password_mode(o, true);
      lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLL_ON_FOCUS);
      lv_obj_add_event_cb(o, focus, LV_EVENT_SHORT_CLICKED, nullptr);
      return o;
    };

    label(content, "Scale", 8, 30, 195, &lv_font_montserrat_14);
    label(content, "Scale host", 211, 30, 221, &lv_font_montserrat_14);

    scale_slot_dropdown = lv_dropdown_create(content);
    lv_obj_set_pos(scale_slot_dropdown, 8, 46);
    lv_obj_set_size(scale_slot_dropdown, 195, 40);
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

    scale_host_dropdown = lv_dropdown_create(content);
    lv_obj_set_pos(scale_host_dropdown, 211, 46);
    lv_obj_set_size(scale_host_dropdown, 221, 40);
    lv_obj_add_event_cb(scale_host_dropdown, scale_host_selection_changed,
                        LV_EVENT_VALUE_CHANGED, nullptr);

    button(content, "Scan Wi-Fi", 8, 94, 120, scan);
    button(content, "Find scale", 136, 94, 120, discover);

    scale_manual_label =
        label(content, "", 0, 0, 1, &lv_font_montserrat_14);
    scale_manual_host = lv_textarea_create(content);
    lv_obj_set_pos(scale_manual_host, 264, 97);
    lv_obj_set_size(scale_manual_host, 168, 40);
    lv_textarea_set_one_line(scale_manual_host, true);
    lv_textarea_set_max_length(scale_manual_host, 127);
    lv_textarea_set_placeholder_text(scale_manual_host, "Manual host");
    lv_textarea_set_text(scale_manual_host, scale_hosts_ui[setup_scale_slot]);
    lv_obj_remove_flag(scale_manual_host, LV_OBJ_FLAG_SCROLL_ON_FOCUS);
    lv_obj_add_event_cb(scale_manual_host, focus, LV_EVENT_SHORT_CLICKED,
                        nullptr);

    load_saved_host_options();

    networks = lv_dropdown_create(content);
    lv_obj_set_pos(networks, 8, 144);
    lv_obj_set_size(networks, 424, 36);
    lv_dropdown_set_options(networks, "Select Wi-Fi network");
    lv_obj_add_event_cb(networks, select_network, LV_EVENT_VALUE_CHANGED,
                        nullptr);

    fields[0] =
        compact_setup_field("SSID", initial.ssid, 8, 186, 207, false, 32);
    fields[1] =
        compact_setup_field("Password", initial.password, 225, 186, 207, true, 64);

    lv_obj_set_style_pad_right(fields[1], 58, LV_PART_MAIN);

    lv_obj_t *password_toggle = lv_button_create(content);
    lv_obj_set_pos(password_toggle, 378, 207);
    lv_obj_set_size(password_toggle, 50, 32);
    lv_obj_set_style_bg_color(password_toggle, lv_color_hex(0x2a3945), 0);
    lv_obj_set_style_bg_opa(password_toggle, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(password_toggle, lv_color_hex(0x587181), 0);
    lv_obj_set_style_border_width(password_toggle, 1, 0);
    lv_obj_set_style_radius(password_toggle, 10, 0);
    lv_obj_set_style_pad_all(password_toggle, 0, 0);

    lv_obj_t *password_toggle_text = lv_label_create(password_toggle);
    lv_label_set_text(password_toggle_text, "SHOW");
    lv_obj_set_style_text_font(password_toggle_text, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(password_toggle_text, lv_color_hex(TEXT), 0);
    lv_obj_center(password_toggle_text);

    lv_obj_add_event_cb(password_toggle, toggle_password_visibility,
                        LV_EVENT_CLICKED, fields[1]);

    label(content, "Brightness", 8, 250, 92, &lv_font_montserrat_14);
    fields[3] = lv_slider_create(content);
    lv_obj_set_pos(fields[3], 108, 258);
    lv_obj_set_size(fields[3], 324, 15);
    lv_slider_set_range(fields[3], 10, 100);
    lv_slider_set_value(fields[3], initial.brightness, LV_ANIM_OFF);
    lv_obj_add_event_cb(fields[3], brightness, LV_EVENT_VALUE_CHANGED, nullptr);

    button(content, "Save & connect", 8, 284, 207, save_settings);
    button(content, "Remove pairing", 225, 284, 207, forget);
  } else {
    // Page 4 belongs entirely to the new OTA UI. Render it immediately rather
    // than drawing the legacy Firmware & diagnostics page first.
    touchscreen_update_page_render_now();
  }
}
} // namespace

// Called from the pairing timeout overlay while already in LVGL UI context.
// Clear the legacy pairing state and redraw Setup underneath the overlay.
void touchscreen_pairing_timeout_cleanup() {
  pair_code[0] = 0;
  if (page_id == 3)
    build(3);
}

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
  content = lv_obj_create(root);
  lv_obj_set_pos(content, 12, 8);
  lv_obj_set_size(content, 456, 394);
  lv_obj_set_scroll_dir(content, LV_DIR_VER);
  lv_obj_remove_flag(content, LV_OBJ_FLAG_SCROLL_ELASTIC);
  lv_obj_set_style_bg_color(content, lv_color_hex(CARD), 0);
  lv_obj_set_style_border_width(content, 0, 0);
  lv_obj_set_style_pad_all(content, 6, 0);
  // Transient messages remain available above the controls.
  notice = label(root, "", 18, 404, 444, &lv_font_montserrat_14);
  lv_obj_set_height(notice, 24);
  lv_label_set_long_mode(notice, LV_LABEL_LONG_DOT);

  // Persistent connection state gets a compact bubble between the two
  // bottom controls instead of occupying the full width.
  connection_badge = label(root, "Offline", 211, 438, 82,
                           &lv_font_montserrat_14);
  lv_obj_set_height(connection_badge, 28);
  lv_obj_set_style_text_align(connection_badge, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_style_text_color(connection_badge, lv_color_hex(TEXT), 0);
  lv_obj_set_style_bg_color(connection_badge, lv_color_hex(0x7a3b3b), 0);
  lv_obj_set_style_bg_opa(connection_badge, LV_OPA_70, 0);
  lv_obj_set_style_radius(connection_badge, 14, 0);
  lv_obj_set_style_pad_top(connection_badge, 5, 0);
  // Keep only Home / Switch Scale on the main screen. All management pages
  // live in the right-side drawer.
  home_nav_button = button(root, "Home", 8, 432, 196, nav,
                           reinterpret_cast<void *>(static_cast<intptr_t>(0)));
  lv_obj_set_size(home_nav_button, 196, 40);
  lv_obj_set_style_bg_color(home_nav_button, lv_color_hex(0x2a3945), 0);
  lv_obj_set_style_bg_opa(home_nav_button, LV_OPA_COVER, 0);
  lv_obj_set_style_border_color(home_nav_button, lv_color_hex(0x587181), 0);
  lv_obj_set_style_border_width(home_nav_button, 1, 0);
  lv_obj_set_style_radius(home_nav_button, 15, 0);
  if (lv_obj_t *home_text = lv_obj_get_child(home_nav_button, 0))
    lv_obj_set_style_text_color(home_text, lv_color_hex(TEXT), 0);
  ensure_scale_menu(root);
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
  update_home_nav_button();
  if (connection_badge) {
    lv_label_set_text(connection_badge, current.online ? "Connected" : "Offline");
    lv_obj_set_style_bg_color(
        connection_badge,
        lv_color_hex(current.online ? 0x247a5a : 0x7a3b3b), 0);
  }
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
  if (!s || !strcmp(s, "Connected to scale") || !strcmp(s, "Not connected"))
    message("");
  else
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

void ui_discovered_options_for_generation(const char *options,
                                          uint32_t generation) {
  if (!bsp_display_lock(1000))
    return;
  if (!touchscreen_ui_generation_accepts(&content_generation, generation) ||
      page_id != 3 || !scale_host_dropdown) {
    ESP_LOGI("display",
             "Ignoring stale scale discovery result generation=%lu current=%lu page=%d",
             (unsigned long)generation,
             (unsigned long)touchscreen_ui_generation_current(&content_generation),
             page_id);
    bsp_display_unlock();
    return;
  }
  snprintf(discovered_scale_options, sizeof(discovered_scale_options), "%s",
           options && options[0] ? options : "Manual IP / hostname...");
  lv_dropdown_set_options(scale_host_dropdown, discovered_scale_options);
  select_host_option_for_slot(true);
  message("Choose a scale from the list, or use Manual IP / hostname.");
  bsp_display_unlock();
}

void ui_networks(const char *options, uint32_t generation) {
  if (!bsp_display_lock(1000))
    return;
  if (!touchscreen_ui_generation_accepts(&content_generation, generation) ||
      page_id != 3 || !networks) {
    ESP_LOGI("display",
             "Ignoring stale Wi-Fi scan result generation=%lu current=%lu page=%d",
             (unsigned long)generation,
             (unsigned long)touchscreen_ui_generation_current(&content_generation),
             page_id);
    bsp_display_unlock();
    return;
  }
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


bool touchscreen_active_scale_paired(void) {
  return scale_paired_ui[active_scale_ui < 2 ? active_scale_ui : 0];
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
