#include "app.h"
#include "ui_lifetime.h"
#include "ui_text.h"
#include "esp_lcd_touch.h"
#include "board.h"

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
// custom multi-view Home renderer instead of the legacy Home widgets.
void touchscreen_home_render_now();
void touchscreen_home_set_menu_open(bool open);
void touchscreen_home_set_keyboard_open(bool open);
// Firmware update UI is rendered directly by this module.
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
// Setup widgets. Async scan results check these before writing.
lv_obj_t *scale_list = nullptr;
lv_obj_t *scale_manual_host = nullptr;
lv_obj_t *selected_list_row = nullptr;
lv_obj_t *wifi_status_label = nullptr;
lv_obj_t *wifi_status_dot = nullptr;
bool wifi_status_shown = false;
lv_obj_t *capacity_dropdown = nullptr;
lv_obj_t *home_nav_button = nullptr;
lv_obj_t *menu_handle_button = nullptr;
lv_obj_t *menu_update_badge = nullptr;
lv_obj_t *menu_scrim = nullptr;
lv_obj_t *menu_panel = nullptr;
lv_obj_t *qr_fullscreen = nullptr;
lv_obj_t *connection_badge = nullptr;
lv_obj_t *connection_badge_text = nullptr;
lv_obj_t *connection_badge_dot = nullptr;
bool touchscreen_update_available_ui = false;
// TOUCH_DRAWER_V1
// TOUCH_SHELL_POLISH_V3
// TOUCH_LAYOUT_POLISH_V4
// TOUCH_DRAWER_REFINEMENTS_V2_4
char discovered_scale_options[800] = "Manual IP / hostname...";
TouchscreenUiGeneration content_generation{1};
bool ui_initialized = false;
lv_timer_t *notice_clear_timer = nullptr;
const uint32_t BG = 0x101c26, CARD = 0x203441, ACCENT = 0x54d6bf,
               TEXT = 0xf2f6f8;
// TOUCH_BUTTON_STYLE_V5
// TOUCH_TRANSIENT_NOTICE_V6
// TOUCH_HOME_NOTICE_REMOVAL_V7
// Page ids. Setup is a hub that leads to the Wi-Fi and Scales pages.
enum : int {
  PAGE_HOME = 0,
  PAGE_KEG = 1,
  PAGE_CALIBRATE = 2,
  PAGE_SETUP = 3,
  PAGE_UPDATE = 4,
  PAGE_WIFI = 5,
  PAGE_SCALES = 6,
  PAGE_SCALE_CONNECT = 7,
};
void build(int page);
void render_update_page();
void focus(lv_event_t *e);
void nav(lv_event_t *e);
void open_keyboard(lv_obj_t *field);
void network_row_clicked(lv_event_t *e);
void scale_row_clicked(lv_event_t *e);

void clear_notice_locked() {
  if (notice_clear_timer) {
    lv_timer_delete(notice_clear_timer);
    notice_clear_timer = nullptr;
  }
  if (notice)
    touchscreen_text::label_set_text(notice, "");
}

void clear_notice_timer_cb(lv_timer_t *timer) {
  if (timer == notice_clear_timer)
    notice_clear_timer = nullptr;
  if (notice)
    touchscreen_text::label_set_text(notice, "");
  lv_timer_delete(timer);
}

void message(const char *s) {
  clear_notice_locked();
  // Home has its own full-screen renderer. Never place the legacy notice strip
  // over it; Home confirmations are reflected directly by the updated data.
  if (page_id == 0) {
    if (notice)
      lv_obj_add_flag(notice, LV_OBJ_FLAG_HIDDEN);
    return;
  }
  if (notice) {
    lv_obj_remove_flag(notice, LV_OBJ_FLAG_HIDDEN);
    touchscreen_text::label_set_text(notice, s ? s : "");
  }
}

void transient_message(const char *s, uint32_t duration_ms = 2200) {
  message(s);
  if (page_id != 0 && notice && s && s[0])
    notice_clear_timer = lv_timer_create(clear_notice_timer_cb, duration_ms, nullptr);
}
bool submit(const char *kind, cJSON *json) {
  Action a{};
  a.ui_generation = touchscreen_ui_generation_current(&content_generation);
  snprintf(a.kind, sizeof(a.kind), "%s", kind);
  char *s = json ? cJSON_PrintUnformatted(json) : nullptr;
  bool valid = !s || strlen(s) < sizeof(a.body);
  // Over-length bodies are rejected via `valid` below; copy never overflows.
  strlcpy(a.body, s ? s : "{}", sizeof(a.body));
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
  touchscreen_text::label_set_text(o, s);
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
  lv_obj_set_style_bg_color(o, lv_color_hex(0x173b55), 0);
  lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
  lv_obj_set_style_border_color(o, lv_color_hex(0x55b7e8), 0);
  lv_obj_set_style_border_width(o, 1, 0);
  lv_obj_set_style_radius(o, 15, 0);

  // Give taps an obvious but restrained pressed-state response.
  lv_obj_set_style_bg_color(o, lv_color_hex(0x276990), LV_STATE_PRESSED);
  lv_obj_set_style_border_color(o, lv_color_hex(ACCENT), LV_STATE_PRESSED);

  auto t = lv_label_create(o);
  touchscreen_text::label_set_text(t, s);
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

  lv_obj_t *root = touchscreen_ui_root();
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
  lv_obj_set_style_bg_color(back, lv_color_hex(0x173b55), 0);
  lv_obj_set_style_border_color(back, lv_color_hex(0x55b7e8), 0);
  lv_obj_set_style_border_width(back, 1, 0);
  lv_obj_set_style_radius(back, 20, 0);
  if (lv_obj_t *back_text = lv_obj_get_child(back, 0))
    lv_obj_set_style_text_color(back_text, lv_color_hex(TEXT), 0);
  lv_obj_set_ext_click_area(back, 6);

  lv_obj_move_foreground(qr_fullscreen);
}

void refresh_menu_update_indicator() {
  if (!menu_update_badge)
    return;
  if (touchscreen_update_available_ui) {
    lv_obj_remove_flag(menu_update_badge, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(menu_update_badge);
  } else {
    lv_obj_add_flag(menu_update_badge, LV_OBJ_FLAG_HIDDEN);
  }
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
  lv_obj_set_style_bg_color(close, lv_color_hex(0x173b55), 0);
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
    lv_obj_set_style_bg_color(b, lv_color_hex(0x173b55), 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(b, lv_color_hex(0x55b7e8), 0);
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
  lv_obj_t *update_button =
      drawer_button(touchscreen_update_available_ui ? "Update Available"
                                                    : "Update / Diagnostics",
                    first_y + gap * 3, 4);
  if (touchscreen_update_available_ui) {
    lv_obj_set_style_border_color(update_button, lv_color_hex(0xffb347), 0);
    lv_obj_set_style_border_width(update_button, 2, 0);
  }
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
    lv_obj_set_style_bg_color(menu_handle_button, lv_color_hex(0x173b55), 0);
    lv_obj_set_style_bg_opa(menu_handle_button, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(menu_handle_button, lv_color_hex(0x55b7e8), 0);
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

    // Reuse the OTA result already maintained by touchscreen_ota.cpp. The
    // badge is purely an attention cue; tapping the hamburger still opens the
    // normal drawer, where Update Available leads to the firmware page.
    menu_update_badge = lv_obj_create(menu_handle_button);
    lv_obj_set_size(menu_update_badge, 16, 16);
    lv_obj_align(menu_update_badge, LV_ALIGN_TOP_RIGHT, 0, 0);
    lv_obj_remove_flag(menu_update_badge, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(menu_update_badge, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_color(menu_update_badge, lv_color_hex(0xffb347), 0);
    lv_obj_set_style_bg_opa(menu_update_badge, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(menu_update_badge, 0, 0);
    lv_obj_set_style_radius(menu_update_badge, 8, 0);
    lv_obj_set_style_pad_all(menu_update_badge, 0, 0);
    lv_obj_t *update_mark = lv_label_create(menu_update_badge);
    touchscreen_text::label_set_text(update_mark, "!");
    lv_obj_set_style_text_font(update_mark, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(update_mark, lv_color_hex(BG), 0);
    lv_obj_center(update_mark);
    refresh_menu_update_indicator();

    lv_obj_add_event_cb(menu_handle_button, open_scale_menu, LV_EVENT_CLICKED,
                        nullptr);
  }

  refresh_menu_update_indicator();
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

  touchscreen_text::label_set_text(text, caption);
  lv_label_set_long_mode(text, LV_LABEL_LONG_DOT);
  lv_obj_set_width(text, lv_obj_get_width(home_nav_button) - 18);
  lv_obj_set_height(text, 20);
  lv_obj_set_style_text_font(text, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_align(text, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_center(text);
}

bool is_manual_host_option(const char *text) {
  return text && !strcmp(text, "Manual IP / hostname...");
}

const uint32_t CARD_BG = 0x173247, CARD_BORDER = 0x55b7e8, MUTED = 0x9fb3bf,
               STATUS_OK = 0x42d3a2, STATUS_WAIT = 0xffb347,
               STATUS_OFF = 0x8aa0ad;

void *page_data(int page) {
  return reinterpret_cast<void *>(static_cast<intptr_t>(page));
}

// Forget every Setup widget pointer before its parent is cleaned, so a late
// Wi-Fi scan or scale search can never write into a deleted object.
void reset_setup_widgets() {
  memset(fields, 0, sizeof(fields));
  networks = nullptr;
  scale_list = nullptr;
  scale_manual_host = nullptr;
  selected_list_row = nullptr;
  wifi_status_label = nullptr;
  wifi_status_dot = nullptr;
}

// LVGL symbol glyphs sit in the private-use range that the text sanitizer
// replaces with '?', so only constant LV_SYMBOL_* strings are set directly.
lv_obj_t *symbol(lv_obj_t *parent, const char *glyph, int x, int y,
                 uint32_t color = TEXT,
                 const lv_font_t *font = &lv_font_montserrat_20) {
  auto o = lv_label_create(parent);
  lv_label_set_text(o, glyph);
  lv_obj_set_pos(o, x, y);
  lv_obj_set_style_text_font(o, font, 0);
  lv_obj_set_style_text_color(o, lv_color_hex(color), 0);
  return o;
}

lv_obj_t *status_dot(lv_obj_t *parent, int x, int y, uint32_t color) {
  auto dot = lv_obj_create(parent);
  lv_obj_set_pos(dot, x, y);
  lv_obj_set_size(dot, 10, 10);
  lv_obj_remove_flag(dot, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_remove_flag(dot, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_style_bg_color(dot, lv_color_hex(color), 0);
  lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(dot, 0, 0);
  lv_obj_set_style_radius(dot, 5, 0);
  lv_obj_set_style_pad_all(dot, 0, 0);
  return dot;
}

// Rounded panel used by the Setup hub and Scales page. With a callback the
// whole card is one large touch target.
lv_obj_t *setup_card(int y, int height, lv_event_cb_t cb = nullptr,
                     void *data = nullptr) {
  auto card = lv_obj_create(content);
  lv_obj_set_pos(card, 8, y);
  lv_obj_set_size(card, 424, height);
  lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_style_bg_color(card, lv_color_hex(CARD_BG), 0);
  lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
  lv_obj_set_style_border_color(card, lv_color_hex(CARD_BORDER), 0);
  lv_obj_set_style_border_width(card, 1, 0);
  lv_obj_set_style_radius(card, 15, 0);
  lv_obj_set_style_pad_all(card, 0, 0);
  if (cb) {
    lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_color(card, lv_color_hex(0x276990), LV_STATE_PRESSED);
    lv_obj_set_style_border_color(card, lv_color_hex(ACCENT), LV_STATE_PRESSED);
    lv_obj_add_event_cb(card, cb, LV_EVENT_CLICKED, data);
  } else {
    lv_obj_remove_flag(card, LV_OBJ_FLAG_CLICKABLE);
  }
  return card;
}

// "< Back" plus the page title, shared by every Setup sub-page.
void page_header(const char *title, int back_page) {
  auto back = button(content, "Back", 8, 0, 96, nav, page_data(back_page));
  lv_obj_set_height(back, 40);
  if (lv_obj_t *text = lv_obj_get_child(back, 0)) {
    lv_label_set_text(text, LV_SYMBOL_LEFT "  Back");
    lv_obj_center(text);
  }
  lv_obj_set_ext_click_area(back, 6);
  label(content, title, 118, 6, 314, &lv_font_montserrat_24);
}

lv_obj_t *small_button(lv_obj_t *parent, const char *text, int x, int y,
                       int width, lv_event_cb_t cb, void *data = nullptr) {
  auto b = button(parent, text, x, y, width, cb, data);
  lv_obj_set_height(b, 36);
  if (lv_obj_t *t = lv_obj_get_child(b, 0)) {
    lv_obj_set_style_text_font(t, &lv_font_montserrat_14, 0);
    lv_obj_center(t);
  }
  return b;
}

lv_obj_t *setup_field(const char *title, const char *value, int x, int y,
                      int width, bool password = false, int limit = 64) {
  label(content, title, x, y, width, &lv_font_montserrat_14);
  auto o = lv_textarea_create(content);
  lv_obj_set_pos(o, x, y + 17);
  lv_obj_set_size(o, width, 40);
  lv_textarea_set_one_line(o, true);
  lv_textarea_set_max_length(o, limit);
  touchscreen_text::textarea_set_text(o, value);
  if (password)
    lv_textarea_set_password_mode(o, true);
  lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLL_ON_FOCUS);
  lv_obj_add_event_cb(o, focus, LV_EVENT_SHORT_CLICKED, nullptr);
  return o;
}

// Scrollable list of tappable rows (Wi-Fi networks, discovered scales).
lv_obj_t *list_box(int y, int height) {
  auto box = lv_obj_create(content);
  lv_obj_set_pos(box, 8, y);
  lv_obj_set_size(box, 424, height);
  lv_obj_set_scroll_dir(box, LV_DIR_VER);
  lv_obj_set_style_bg_color(box, lv_color_hex(BG), 0);
  lv_obj_set_style_bg_opa(box, LV_OPA_COVER, 0);
  lv_obj_set_style_border_color(box, lv_color_hex(0x2d4a5e), 0);
  lv_obj_set_style_border_width(box, 1, 0);
  lv_obj_set_style_radius(box, 12, 0);
  lv_obj_set_style_pad_all(box, 5, 0);
  lv_obj_set_style_pad_row(box, 5, 0);
  lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
  return box;
}

void list_placeholder(lv_obj_t *box, const char *text) {
  if (!box)
    return;
  auto o = lv_label_create(box);
  touchscreen_text::label_set_text(o, text);
  lv_obj_set_width(o, lv_pct(100));
  lv_label_set_long_mode(o, LV_LABEL_LONG_WRAP);
  lv_obj_set_style_text_font(o, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(o, lv_color_hex(MUTED), 0);
  lv_obj_set_style_pad_all(o, 6, 0);
}

// Row label child 0 always holds the exact value (SSID or host name).
lv_obj_t *list_row(lv_obj_t *box, const char *text, lv_event_cb_t cb,
                   const char *note = nullptr) {
  auto row = lv_button_create(box);
  lv_obj_set_size(row, lv_pct(100), 40);
  lv_obj_set_style_bg_color(row, lv_color_hex(0x173b55), 0);
  lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(row, 0, 0);
  lv_obj_set_style_radius(row, 10, 0);
  lv_obj_set_style_shadow_width(row, 0, 0);
  lv_obj_set_style_pad_all(row, 0, 0);
  lv_obj_set_style_bg_color(row, lv_color_hex(0x276990), LV_STATE_PRESSED);
  lv_obj_set_style_bg_color(row, lv_color_hex(0x1f5a7a), LV_STATE_CHECKED);
  lv_obj_set_style_border_color(row, lv_color_hex(ACCENT), LV_STATE_CHECKED);
  lv_obj_set_style_border_width(row, 2, LV_STATE_CHECKED);
  lv_obj_set_style_bg_opa(row, LV_OPA_50, LV_STATE_DISABLED);

  auto t = lv_label_create(row);
  touchscreen_text::label_set_text(t, text);
  lv_obj_set_width(t, note ? 290 : 380);
  lv_label_set_long_mode(t, LV_LABEL_LONG_DOT);
  lv_obj_set_style_text_color(t, lv_color_hex(TEXT), 0);
  lv_obj_align(t, LV_ALIGN_LEFT_MID, 12, 0);

  if (note) {
    auto n = lv_label_create(row);
    touchscreen_text::label_set_text(n, note);
    lv_obj_set_style_text_font(n, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(n, lv_color_hex(MUTED), 0);
    lv_obj_align(n, LV_ALIGN_RIGHT_MID, -12, 0);
  }
  if (cb)
    lv_obj_add_event_cb(row, cb, LV_EVENT_CLICKED, nullptr);
  return row;
}

void mark_selected_row(lv_obj_t *row) {
  if (selected_list_row)
    lv_obj_remove_state(selected_list_row, LV_STATE_CHECKED);
  selected_list_row = row;
  if (row)
    lv_obj_add_state(row, LV_STATE_CHECKED);
}

const char *row_value(lv_obj_t *row) {
  lv_obj_t *t = row ? lv_obj_get_child(row, 0) : nullptr;
  return t ? lv_label_get_text(t) : "";
}

// Copies line `index` of a newline-separated option list; false at the end.
bool option_line(const char *options, int index, char *out, size_t size) {
  if (!options || !size)
    return false;
  const char *p = options;
  for (int i = 0; i < index; ++i) {
    p = strchr(p, '\n');
    if (!p)
      return false;
    ++p;
  }
  const char *end = strchr(p, '\n');
  size_t len = end ? (size_t)(end - p) : strlen(p);
  if (!len && !end && !*p)
    return false;
  if (len >= size)
    len = size - 1;
  memcpy(out, p, len);
  out[len] = 0;
  return true;
}

const char *scale_status(uint8_t slot, uint32_t *color) {
  if (!scale_hosts_ui[slot][0]) {
    *color = STATUS_OFF;
    return "Not set up";
  }
  if (slot == active_scale_ui && current.online) {
    *color = STATUS_OK;
    return "Connected";
  }
  if (scale_paired_ui[slot]) {
    *color = STATUS_WAIT;
    return slot == active_scale_ui ? "Offline" : "Paired";
  }
  *color = STATUS_WAIT;
  return "Not paired";
}

// Wi-Fi status line on the Setup hub or the Wi-Fi page. The 1 s UI timer
// calls this again when the connection comes up or drops.
void render_wifi_status() {
  if (!wifi_status_label)
    return;
  const bool up = touchscreen_wifi_connected();
  wifi_status_shown = up;
  char text[96];
  if (!initial.ssid[0])
    snprintf(text, sizeof(text), "%s",
             page_id == PAGE_SETUP ? "Not set up - tap to choose a network"
                                   : "Not connected to Wi-Fi yet");
  else if (page_id == PAGE_SETUP)
    snprintf(text, sizeof(text), "%s - %s", initial.ssid,
             up ? "Connected" : "Not connected");
  else
    snprintf(text, sizeof(text), "%s %s",
             up ? "Connected to" : "Not connected:", initial.ssid);
  touchscreen_text::label_set_text(wifi_status_label, text);
  if (wifi_status_dot)
    lv_obj_set_style_bg_color(
        wifi_status_dot,
        lv_color_hex(!initial.ssid[0] ? STATUS_OFF
                     : up             ? STATUS_OK
                                      : STATUS_WAIT),
        0);
}

uint8_t saved_brightness() {
  return initial.brightness >= 10 && initial.brightness <= 100
             ? initial.brightness
             : 85;
}

// Every connection change goes through the existing "settings" action. Values
// the page does not edit are sent back unchanged, so the main task applies
// only what actually changed (Wi-Fi, or one scale's host).
bool submit_connection(uint8_t slot, const char *ssid, const char *password,
                       const char *host) {
  auto o = cJSON_CreateObject();
  cJSON_AddNumberToObject(o, "slot", slot);
  cJSON_AddStringToObject(o, "ssid", ssid);
  cJSON_AddStringToObject(o, "password", password);
  cJSON_AddStringToObject(o, "host", host);
  cJSON_AddNumberToObject(o, "brightness", saved_brightness());
  const bool sent = submit("settings", o);
  cJSON_Delete(o);
  return sent;
}

void toggle_password_visibility(lv_event_t *event) {
  auto button = static_cast<lv_obj_t *>(lv_event_get_target(event));
  auto password = static_cast<lv_obj_t *>(lv_event_get_user_data(event));
  if (!button || !password)
    return;

  const bool currently_hidden = lv_textarea_get_password_mode(password);
  lv_textarea_set_password_mode(password, !currently_hidden);

  if (lv_obj_t *button_text = lv_obj_get_child(button, 0))
    touchscreen_text::label_set_text(button_text, currently_hidden ? "HIDE" : "SHOW");

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
  open_keyboard(static_cast<lv_obj_t *>(lv_event_get_target(e)));
}
void open_keyboard(lv_obj_t *field) {
  if (!field)
    return;
  touchscreen_home_set_keyboard_open(true);

  if (!keyboard) {
    keyboard = lv_keyboard_create(touchscreen_ui_root());
    lv_obj_set_size(keyboard, 480, 200);
    lv_obj_align(keyboard, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_add_event_cb(keyboard, keyboard_event, LV_EVENT_READY, nullptr);
    lv_obj_add_event_cb(keyboard, keyboard_event, LV_EVENT_CANCEL, nullptr);
  }

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
  touchscreen_text::textarea_set_text(o, value);
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
  touchscreen_text::textarea_set_text(fields[1], value);
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
// ---- Wi-Fi page -----------------------------------------------------------
void fill_network_list(const char *options) {
  if (!networks)
    return;
  lv_obj_clean(networks);
  selected_list_row = nullptr;
  int added = 0;
  char ssid[40];
  for (int i = 0; option_line(options, i, ssid, sizeof(ssid)); ++i) {
    if (!ssid[0] || !strcmp(ssid, "No networks found"))
      continue;
    // One row per network name, even when several access points share it.
    bool duplicate = false;
    const uint32_t rows = lv_obj_get_child_count(networks);
    for (uint32_t r = 0; r < rows && !duplicate; ++r)
      duplicate = !strcmp(row_value(lv_obj_get_child(networks, r)), ssid);
    if (duplicate)
      continue;
    const bool saved = initial.ssid[0] && !strcmp(ssid, initial.ssid);
    auto row = list_row(networks, ssid, network_row_clicked,
                        saved ? "Saved" : nullptr);
    if (saved)
      mark_selected_row(row);
    ++added;
  }
  if (!added)
    list_placeholder(networks,
                     "No networks found. Tap Rescan, or type the network "
                     "name below.");
}

void network_row_clicked(lv_event_t *e) {
  auto row = static_cast<lv_obj_t *>(lv_event_get_current_target(e));
  if (!row || !fields[0] || !fields[1])
    return;
  mark_selected_row(row);
  const char *ssid = row_value(row);
  const bool same_as_saved = !strcmp(ssid, initial.ssid);
  touchscreen_text::textarea_set_text(fields[0], ssid);
  touchscreen_text::textarea_set_text(fields[1],
                                      same_as_saved ? initial.password : "");
  // The next thing the user needs is the password.
  open_keyboard(fields[1]);
}

void scan(lv_event_t *) {
  dismiss_keyboard();
  if (networks) {
    lv_obj_clean(networks);
    selected_list_row = nullptr;
    list_placeholder(networks, "Scanning for networks…");
  }
  submit("scan_wifi", nullptr);
}

void wifi_connect(lv_event_t *) {
  if (!fields[0] || !fields[1])
    return;
  char ssid[33];
  char password[65];
  snprintf(ssid, sizeof(ssid), "%s", lv_textarea_get_text(fields[0]));
  snprintf(password, sizeof(password), "%s", lv_textarea_get_text(fields[1]));
  if (!ssid[0]) {
    message("Choose a network, or type its name");
    return;
  }
  dismiss_keyboard();
  const uint8_t slot = active_scale_ui < 2 ? active_scale_ui : 0;
  if (submit_connection(slot, ssid, password, scale_hosts_ui[slot]))
    message("Connecting to Wi-Fi…");
}

// ---- Scales pages ---------------------------------------------------------
void fill_scale_list(const char *options) {
  if (!scale_list)
    return;
  lv_obj_clean(scale_list);
  selected_list_row = nullptr;
  const uint8_t other = setup_scale_slot == 0 ? 1 : 0;
  int added = 0;
  char host[128];
  for (int i = 0; option_line(options, i, host, sizeof(host)); ++i) {
    if (!host[0] || is_manual_host_option(host))
      continue;
    const bool taken =
        scale_hosts_ui[other][0] && !strcmp(host, scale_hosts_ui[other]);
    char note[16] = {};
    if (taken)
      snprintf(note, sizeof(note), "Scale %u", (unsigned)(other + 1));
    auto row = list_row(scale_list, host, taken ? nullptr : scale_row_clicked,
                        taken ? note : nullptr);
    if (taken)
      lv_obj_add_state(row, LV_STATE_DISABLED);
    else if (!strcmp(host, scale_hosts_ui[setup_scale_slot]))
      mark_selected_row(row);
    ++added;
  }
  if (!added)
    list_placeholder(scale_list,
                     "No scales found. Check the scale is powered on and on "
                     "the same Wi-Fi, or type its address below.");
}

void scale_row_clicked(lv_event_t *e) {
  auto row = static_cast<lv_obj_t *>(lv_event_get_current_target(e));
  if (!row || !scale_manual_host)
    return;
  dismiss_keyboard();
  mark_selected_row(row);
  touchscreen_text::textarea_set_text(scale_manual_host, row_value(row));
}

void discover(lv_event_t *) {
  dismiss_keyboard();
  if (scale_list) {
    lv_obj_clean(scale_list);
    selected_list_row = nullptr;
    list_placeholder(scale_list, "Searching for scales…");
  }
  auto o = cJSON_CreateObject();
  cJSON_AddNumberToObject(o, "slot", setup_scale_slot);
  submit("discover", o);
  cJSON_Delete(o);
}

void open_scale_connect(lv_event_t *e) {
  const int slot =
      static_cast<int>(reinterpret_cast<intptr_t>(lv_event_get_user_data(e)));
  setup_scale_slot = slot == 1 ? 1 : 0;
  build(PAGE_SCALE_CONNECT);
}

void scale_connect(lv_event_t *) {
  if (!scale_manual_host)
    return;
  // Accept a pasted URL: keep only the host name or IP address.
  char host[128];
  const char *text = lv_textarea_get_text(scale_manual_host);
  while (*text == ' ')
    ++text;
  if (!strncmp(text, "http://", 7))
    text += 7;
  else if (!strncmp(text, "https://", 8))
    text += 8;
  snprintf(host, sizeof(host), "%s", text);
  for (char *p = host; *p; ++p) {
    if (*p == '/' || *p == ' ') {
      *p = 0;
      break;
    }
  }
  if (!host[0]) {
    message("Tap a scale, or type its address");
    return;
  }
  dismiss_keyboard();
  if (submit_connection(setup_scale_slot, initial.ssid, initial.password,
                        host))
    message("Connecting to scale…");
}

void forget_yes(lv_event_t *) {
  auto o = cJSON_CreateObject();
  cJSON_AddNumberToObject(o, "slot", setup_scale_slot);
  submit("forget", o);
  cJSON_Delete(o);
  build(PAGE_SCALES);
}
void forget(lv_event_t *e) {
  const int slot =
      static_cast<int>(reinterpret_cast<intptr_t>(lv_event_get_user_data(e)));
  setup_scale_slot = slot == 1 ? 1 : 0;
  dismiss_keyboard();
  // A Wi-Fi scan/discovery may complete while this confirmation is visible.
  // Do not let its result write into deleted setup widgets.
  reset_setup_widgets();
  touchscreen_ui_generation_advance(&content_generation);
  lv_obj_clean(content);
  char title[48];
  snprintf(title, sizeof(title), "Remove Scale %u pairing?",
           (unsigned)(setup_scale_slot + 1));
  label(content, title, 8, 12, 424, &lv_font_montserrat_24);
  label(content,
        "This removes the pairing from both the touchscreen and the scale. "
        "Use Add touchscreen on the scale when you are ready to pair again.",
        8, 64, 424);
  button(content, "Remove pairing", 8, 185, 205, forget_yes);
  button(content, "Cancel", 230, 185, 200, nav, page_data(PAGE_SCALES));
}

// ---- Display --------------------------------------------------------------
void brightness(lv_event_t *e) {
  auto slider = static_cast<lv_obj_t *>(lv_event_get_target(e));
  const int32_t value = lv_slider_get_value(slider);
  bsp_display_brightness_set(value);
  if (fields[4])
    touchscreen_text::label_set_text_fmt(fields[4], "%ld%%", (long)value);
}

void brightness_released(lv_event_t *e) {
  auto slider = static_cast<lv_obj_t *>(lv_event_get_target(e));
  const int32_t value = lv_slider_get_value(slider);
  if (value == initial.brightness)
    return;
  initial.brightness = (uint8_t)value;
  auto o = cJSON_CreateObject();
  cJSON_AddNumberToObject(o, "brightness", value);
  submit("brightness", o);
  cJSON_Delete(o);
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
    touchscreen_text::label_set_text(headline,
                      current.name[0] ? current.name : "Set up your keg");
    if (!current.online) {
      touchscreen_text::label_set_text(detail, "--");
      lv_arc_set_value(arc, 0);
      touchscreen_text::label_set_text(connection, "Not connected");
      return;
    }
    if (current.ready)
      touchscreen_text::label_set_text_fmt(detail, "%.0f", (double)floorf(current.servings));
    else
      touchscreen_text::label_set_text(detail, "--");
    lv_arc_set_value(arc, current.ready ? (int)current.percent : 0);
    touchscreen_text::label_set_text_fmt(connection,
                          "Connected\n%.2f gal | %.1f oz | %.2f lb | %s",
                          (double)current.gallons, (double)current.serving,
                          (double)current.weight,
                          current.stable ? "Stable" : "Settling");

  } else if (page_id == 2) {
    touchscreen_text::label_set_text_fmt(
        headline, "Scale: %.3f lb   %s", (double)current.weight,
        current.online ? (current.stable ? "Stable" : "Settling")
                       : "Not connected");
  }
}
void build(int page) {
  dismiss_keyboard();
  clear_notice_locked();
  page_id = page;

  // The old shell reserved 24 px above the footer for notice text. Home now
  // owns that area, so hide the notice and extend content to just above the
  // footer. Other pages retain the original notice strip.
  if (content) {
    lv_obj_set_size(content, 456, page == 0 ? 416 : 394);
  }
  if (notice) {
    if (page == 0)
      lv_obj_add_flag(notice, LV_OBJ_FLAG_HIDDEN);
    else
      lv_obj_remove_flag(notice, LV_OBJ_FLAG_HIDDEN);
  }

  update_home_nav_button();
  touchscreen_ui_generation_advance(&content_generation);
  lv_obj_clean(content);
  lv_obj_scroll_to(content, 0, 0, LV_ANIM_OFF);
  headline = detail = connection = arc = nullptr;
  capacity_dropdown = nullptr;
  reset_setup_widgets();
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
      touchscreen_text::textarea_set_text(o, value);
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
    touchscreen_text::dropdown_set_options(
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
    touchscreen_text::textarea_set_text(fields[1], capacity_value);
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
  } else if (page == PAGE_SETUP) {
    // Setup hub: one card per area, each showing its current state.
    label(content, "Setup", 8, 0, 424, &lv_font_montserrat_24);

    // Wi-Fi
    auto wifi = setup_card(40, 78, nav, page_data(PAGE_WIFI));
    symbol(wifi, LV_SYMBOL_WIFI, 16, 14);
    label(wifi, "Wi-Fi", 52, 11, 300, &lv_font_montserrat_20);
    wifi_status_dot = status_dot(wifi, 52, 49, STATUS_OFF);
    wifi_status_label = label(wifi, "", 70, 44, 316, &lv_font_montserrat_14);
    lv_label_set_long_mode(wifi_status_label, LV_LABEL_LONG_DOT);
    render_wifi_status();
    symbol(wifi, LV_SYMBOL_RIGHT, 396, 28, MUTED);

    // Scales
    auto scales = setup_card(128, 104, nav, page_data(PAGE_SCALES));
    symbol(scales, LV_SYMBOL_LIST, 16, 14);
    label(scales, "Scales", 52, 11, 300, &lv_font_montserrat_20);
    for (uint8_t slot = 0; slot < 2; ++slot) {
      uint32_t color;
      const char *status = scale_status(slot, &color);
      char text[300];
      if (scale_hosts_ui[slot][0])
        snprintf(text, sizeof(text), "Scale %u: %s - %s",
                 (unsigned)(slot + 1), scale_hosts_ui[slot], status);
      else
        snprintf(text, sizeof(text), "Scale %u: %s", (unsigned)(slot + 1),
                 status);
      const int y = 44 + slot * 26;
      status_dot(scales, 52, y + 5, color);
      auto l = label(scales, text, 70, y, 316, &lv_font_montserrat_14);
      lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
    }
    symbol(scales, LV_SYMBOL_RIGHT, 396, 40, MUTED);

    // Display: adjusted in place, no extra page needed.
    auto display = setup_card(242, 92);
    symbol(display, LV_SYMBOL_EYE_OPEN, 16, 14);
    label(display, "Brightness", 52, 11, 260, &lv_font_montserrat_20);
    char percent[8];
    snprintf(percent, sizeof(percent), "%u%%", (unsigned)saved_brightness());
    fields[4] = label(display, percent, 330, 13, 76, &lv_font_montserrat_18);
    lv_obj_set_style_text_align(fields[4], LV_TEXT_ALIGN_RIGHT, 0);
    fields[3] = lv_slider_create(display);
    lv_obj_set_pos(fields[3], 28, 56);
    lv_obj_set_size(fields[3], 368, 15);
    lv_slider_set_range(fields[3], 10, 100);
    lv_slider_set_value(fields[3], saved_brightness(), LV_ANIM_OFF);
    lv_obj_set_ext_click_area(fields[3], 12);
    lv_obj_add_event_cb(fields[3], brightness, LV_EVENT_VALUE_CHANGED, nullptr);
    lv_obj_add_event_cb(fields[3], brightness_released, LV_EVENT_RELEASED,
                        nullptr);
  } else if (page == PAGE_WIFI) {
    page_header("Wi-Fi", PAGE_SETUP);

    wifi_status_dot = status_dot(content, 10, 57, STATUS_OFF);
    wifi_status_label = label(content, "", 28, 52, 404, &lv_font_montserrat_14);
    lv_label_set_long_mode(wifi_status_label, LV_LABEL_LONG_DOT);
    render_wifi_status();

    label(content, "Tap your network", 8, 86, 260, &lv_font_montserrat_14);
    small_button(content, "Rescan", 332, 76, 100, scan);
    networks = list_box(116, 120);

    fields[0] = setup_field("Network name", initial.ssid, 8, 242, 207, false, 32);
    fields[1] = setup_field("Password", initial.password, 225, 242, 207, true, 64);
    lv_obj_set_style_pad_right(fields[1], 58, LV_PART_MAIN);

    lv_obj_t *password_toggle = lv_button_create(content);
    lv_obj_set_pos(password_toggle, 378, 263);
    lv_obj_set_size(password_toggle, 50, 32);
    lv_obj_set_style_bg_color(password_toggle, lv_color_hex(0x173b55), 0);
    lv_obj_set_style_bg_opa(password_toggle, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(password_toggle, lv_color_hex(0x55b7e8), 0);
    lv_obj_set_style_border_width(password_toggle, 1, 0);
    lv_obj_set_style_radius(password_toggle, 10, 0);
    lv_obj_set_style_pad_all(password_toggle, 0, 0);
    lv_obj_t *password_toggle_text = lv_label_create(password_toggle);
    touchscreen_text::label_set_text(password_toggle_text, "SHOW");
    lv_obj_set_style_text_font(password_toggle_text, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(password_toggle_text, lv_color_hex(TEXT), 0);
    lv_obj_center(password_toggle_text);
    lv_obj_add_event_cb(password_toggle, toggle_password_visibility,
                        LV_EVENT_CLICKED, fields[1]);

    button(content, "Connect to Wi-Fi", 8, 314, 424, wifi_connect);

    // A scan takes the radio off the home channel for a couple of seconds and
    // stalls the scale connection, so only scan automatically when Wi-Fi is
    // not working yet. Otherwise show the saved network and wait for Rescan.
    if (initial.ssid[0] && touchscreen_wifi_connected()) {
      fill_network_list(initial.ssid);
      list_placeholder(networks, "Tap Rescan to see other networks.");
    } else {
      scan(nullptr);
    }
  } else if (page == PAGE_SCALES) {
    page_header("Scales", PAGE_SETUP);

    for (uint8_t slot = 0; slot < 2; ++slot) {
      auto card = setup_card(50 + slot * 128, 118);
      char title[16];
      snprintf(title, sizeof(title), "Scale %u", (unsigned)(slot + 1));
      label(card, title, 14, 10, 160, &lv_font_montserrat_20);

      uint32_t color;
      const char *status = scale_status(slot, &color);
      status_dot(card, 270, 17, color);
      label(card, status, 288, 12, 124, &lv_font_montserrat_14);

      auto host = label(card,
                        scale_hosts_ui[slot][0] ? scale_hosts_ui[slot]
                                                : "No scale chosen yet",
                        14, 38, 396, &lv_font_montserrat_14);
      lv_label_set_long_mode(host, LV_LABEL_LONG_DOT);
      if (!scale_hosts_ui[slot][0])
        lv_obj_set_style_text_color(host, lv_color_hex(MUTED), 0);

      void *slot_data = reinterpret_cast<void *>(static_cast<intptr_t>(slot));
      if (!scale_hosts_ui[slot][0]) {
        button(card, "Set up this scale", 12, 62, 398, open_scale_connect,
               slot_data);
      } else if (scale_paired_ui[slot]) {
        button(card, "Change scale", 12, 62, 194, open_scale_connect,
               slot_data);
        button(card, "Remove pairing", 216, 62, 194, forget, slot_data);
      } else {
        button(card, "Change scale", 12, 62, 398, open_scale_connect,
               slot_data);
      }
    }

    auto hint = label(content,
                      "To pair: on the scale's web page, choose Add "
                      "touchscreen.",
                      8, 310, 424, &lv_font_montserrat_14);
    lv_obj_set_style_text_color(hint, lv_color_hex(MUTED), 0);
  } else if (page == PAGE_SCALE_CONNECT) {
    char title[24];
    snprintf(title, sizeof(title), "Scale %u", (unsigned)(setup_scale_slot + 1));
    page_header(title, PAGE_SCALES);

    label(content, "Scales on your Wi-Fi", 8, 56, 260, &lv_font_montserrat_14);
    small_button(content, "Search again", 302, 46, 130, discover);
    scale_list = list_box(88, 124);

    label(content, "Or type its address (IP or name)", 8, 220, 424,
          &lv_font_montserrat_14);
    scale_manual_host = lv_textarea_create(content);
    lv_obj_set_pos(scale_manual_host, 8, 238);
    lv_obj_set_size(scale_manual_host, 424, 40);
    lv_textarea_set_one_line(scale_manual_host, true);
    lv_textarea_set_max_length(scale_manual_host, 127);
    lv_textarea_set_placeholder_text(scale_manual_host,
                                     "e.g. KegScale-1234.local");
    touchscreen_text::textarea_set_text(scale_manual_host,
                                        scale_hosts_ui[setup_scale_slot]);
    lv_obj_remove_flag(scale_manual_host, LV_OBJ_FLAG_SCROLL_ON_FOCUS);
    lv_obj_add_event_cb(scale_manual_host, focus, LV_EVENT_SHORT_CLICKED,
                        nullptr);

    button(content, "Connect", 8, 292, 207, scale_connect);
    button(content, "Cancel", 225, 292, 207, nav, page_data(PAGE_SCALES));

    discover(nullptr);
  } else {
    // Page 4 belongs entirely to the new OTA UI. Render it immediately rather
    // than drawing the legacy Firmware & diagnostics page first.
    render_update_page();
  }
}

enum class OtaView {
  Idle,
  Checking,
  Preparing,
  Current,
  Available,
  Stale,
  Error,
};

OtaView ota_view = OtaView::Idle;
char ota_current[32] = {};
char ota_latest[32] = {};
char ota_error_text[120] = {};
bool update_page_customized = false;
lv_obj_t *ota_channel_dropdown = nullptr;
lv_obj_t *ota_auto_install_switch = nullptr;
lv_obj_t *ota_warning_label = nullptr;
lv_obj_t *ota_last_check_value = nullptr;
lv_obj_t *ota_overlay = nullptr;
lv_obj_t *ota_overlay_title = nullptr;
lv_obj_t *ota_overlay_percent = nullptr;
lv_obj_t *ota_overlay_status = nullptr;
lv_obj_t *ota_overlay_bar = nullptr;
const uint32_t WARNING = 0xffb347;

void copy_text(char *dest, size_t size, const char *source) {
  if (!dest || !size)
    return;
  snprintf(dest, size, "%s", source ? source : "");
}

const char *channel_from_selection(uint16_t selected) {
  if (selected == 0)
    return "production";
  if (selected == 1)
    return "beta";
  return "dev";
}

uint16_t selection_from_channel(const char *channel) {
  if (channel && !strcmp(channel, "production"))
    return 0;
  if (channel && !strcmp(channel, "beta"))
    return 1;
  return 2;
}

const char *channel_display_name(const char *channel) {
  if (channel && !strcmp(channel, "production"))
    return "Production";
  if (channel && !strcmp(channel, "beta"))
    return "Beta";
  return "Development";
}

void format_last_check(char *buffer, size_t size) {
  if (!touchscreen_ota_has_checked()) {
    snprintf(buffer, size, "Not checked yet");
    return;
  }
  uint64_t seconds = touchscreen_ota_last_check_age_seconds();
  if (seconds < 60)
    snprintf(buffer, size, "Just now");
  else if (seconds < 3600)
    snprintf(buffer, size, "%llu min ago",
             (unsigned long long)(seconds / 60));
  else if (seconds < 86400)
    snprintf(buffer, size, "%llu hr ago",
             (unsigned long long)(seconds / 3600));
  else
    snprintf(buffer, size, "%llu day%s ago",
             (unsigned long long)(seconds / 86400),
             seconds / 86400 == 1 ? "" : "s");
}

void clear_ota_overlay() {
  if (ota_overlay)
    lv_obj_delete(ota_overlay);
  ota_overlay = nullptr;
  ota_overlay_title = nullptr;
  ota_overlay_percent = nullptr;
  ota_overlay_status = nullptr;
  ota_overlay_bar = nullptr;
}

void update_channel_warning() {
  if (!ota_channel_dropdown || !ota_warning_label)
    return;
  const uint16_t selected = lv_dropdown_get_selected(ota_channel_dropdown);
  if (selected == 0) {
    touchscreen_text::label_set_text(
        ota_warning_label,
        "Stable releases. Recommended for normal use.");
    lv_obj_set_style_text_color(ota_warning_label, lv_color_hex(ACCENT), 0);
  } else if (selected == 1) {
    touchscreen_text::label_set_text(
        ota_warning_label,
        "Pre-release validation builds. May contain unfinished changes.");
    lv_obj_set_style_text_color(ota_warning_label, lv_color_hex(WARNING), 0);
  } else {
    touchscreen_text::label_set_text(
        ota_warning_label,
        "Experimental builds. Use Development only when instructed.");
    lv_obj_set_style_text_color(ota_warning_label, lv_color_hex(WARNING), 0);
  }
}

void channel_changed(lv_event_t *) { update_channel_warning(); }

void render_update_page();

void check_update(lv_event_t *) {
  ota_view = OtaView::Checking;
  ota_error_text[0] = 0;
  render_update_page();
  esp_err_t e = touchscreen_ota_request(false, true);
  if (e != ESP_OK) {
    ota_view = OtaView::Error;
    copy_text(ota_error_text, sizeof(ota_error_text),
              e == ESP_ERR_INVALID_STATE
                  ? "An update check is already running."
                  : "Could not start the update check. Please try again.");
    render_update_page();
  }
}

void install_update(lv_event_t *) {
  ota_view = OtaView::Preparing;
  render_update_page();
  esp_err_t e = touchscreen_ota_request(true, true);
  if (e != ESP_OK) {
    ota_view = OtaView::Error;
    copy_text(ota_error_text, sizeof(ota_error_text),
              e == ESP_ERR_INVALID_STATE
                  ? "An update operation is already running."
                  : "Could not start the firmware update. Please try again.");
    render_update_page();
  }
}

void save_update_settings(lv_event_t *) {
  if (!ota_channel_dropdown || !ota_auto_install_switch)
    return;
  const char *channel =
      channel_from_selection(lv_dropdown_get_selected(ota_channel_dropdown));
  const bool auto_install =
      lv_obj_has_state(ota_auto_install_switch, LV_STATE_CHECKED);
  esp_err_t e = touchscreen_ota_save_preferences(channel, auto_install);
  if (e != ESP_OK) {
    ota_view = OtaView::Error;
    copy_text(ota_error_text, sizeof(ota_error_text),
              "Could not save update settings.");
    render_update_page();
    return;
  }

  ota_view = OtaView::Checking;
  ota_latest[0] = 0;
  message("Update settings saved");
  render_update_page();
  e = touchscreen_ota_request(false, true);
  if (e != ESP_OK) {
    ota_view = OtaView::Error;
    copy_text(ota_error_text, sizeof(ota_error_text),
              "Settings saved, but the update check could not start.");
    render_update_page();
  }
}
void close_ota_overlay(lv_event_t *) {
  clear_ota_overlay();
  update_page_customized = false;
  build(4);
}

void create_install_overlay(const char *version) {
  clear_ota_overlay();

  ota_overlay = lv_obj_create(touchscreen_ui_root());
  lv_obj_set_pos(ota_overlay, 0, 0);
  lv_obj_set_size(ota_overlay, 480, 480);
  lv_obj_remove_flag(ota_overlay, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(ota_overlay, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_style_bg_color(ota_overlay, lv_color_hex(BG), 0);
  lv_obj_set_style_bg_opa(ota_overlay, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(ota_overlay, 0, 0);
  lv_obj_set_style_radius(ota_overlay, 0, 0);
  lv_obj_set_style_pad_all(ota_overlay, 0, 0);

  ota_overlay_title =
      label(ota_overlay, "Updating Firmware", 24, 42, 432,
            &lv_font_montserrat_24);
  auto version_label =
      label(ota_overlay, version && version[0] ? version : "Preparing update",
            24, 96, 432, &lv_font_montserrat_20);
  lv_obj_set_style_text_color(version_label, lv_color_hex(ACCENT), 0);

  ota_overlay_percent =
      label(ota_overlay, "0%", 24, 150, 432, &lv_font_montserrat_48);
  lv_obj_set_style_text_align(ota_overlay_percent, LV_TEXT_ALIGN_CENTER, 0);

  ota_overlay_bar = lv_bar_create(ota_overlay);
  lv_obj_set_pos(ota_overlay_bar, 36, 225);
  lv_obj_set_size(ota_overlay_bar, 408, 24);
  lv_bar_set_range(ota_overlay_bar, 0, 100);
  lv_bar_set_value(ota_overlay_bar, 0, LV_ANIM_OFF);
  lv_obj_set_style_bg_color(ota_overlay_bar, lv_color_hex(CARD), LV_PART_MAIN);
  lv_obj_set_style_bg_color(ota_overlay_bar, lv_color_hex(ACCENT),
                            LV_PART_INDICATOR);
  lv_obj_set_style_radius(ota_overlay_bar, 12, LV_PART_MAIN);
  lv_obj_set_style_radius(ota_overlay_bar, 12, LV_PART_INDICATOR);

  ota_overlay_status =
      label(ota_overlay, "Preparing secure download...", 36, 285, 408,
            &lv_font_montserrat_18);
  lv_obj_set_style_text_align(ota_overlay_status, LV_TEXT_ALIGN_CENTER, 0);

  auto warning = label(
      ota_overlay,
      "Keep power connected. The touchscreen is locked while firmware is being "
      "downloaded, verified, and installed.",
      36, 350, 408, &lv_font_montserrat_16);
  lv_obj_set_style_text_align(warning, LV_TEXT_ALIGN_CENTER, 0);

  lv_obj_move_foreground(ota_overlay);
}

const char *status_text() {
  switch (ota_view) {
  case OtaView::Checking:
    return "Checking...";
  case OtaView::Preparing:
    return "Preparing update...";
  case OtaView::Current:
    return "Up to date";
  case OtaView::Available:
    return "Update available";
  case OtaView::Stale:
    return "Local build newer";
  case OtaView::Error:
    return "Check failed";
  default:
    return touchscreen_ota_has_checked() ? "Checked" : "Not checked";
  }
}

void render_update_page() {
  if (page_id != 4 || ota_overlay)
    return;

  update_page_customized = true;
  lv_obj_clean(content);
  lv_obj_scroll_to(content, 0, 0, LV_ANIM_OFF);
  headline = detail = connection = arc = networks = nullptr;
  memset(fields, 0, sizeof(fields));
  ota_channel_dropdown = nullptr;
  ota_auto_install_switch = nullptr;
  ota_warning_label = nullptr;
  ota_last_check_value = nullptr;

  OtaPreferences preferences{};
  touchscreen_ota_get_preferences(&preferences);
  const char *running = esp_app_get_description()->version;

  // Everything on this page stays inside the normal 334-pixel content area.
  label(content, "Firmware updates", 8, 0, 424, &lv_font_montserrat_24);

  // Current / Latest / Status summary.
  label(content, "CURRENT", 8, 37, 128, &lv_font_montserrat_14);
  auto current_value =
      label(content, running, 8, 55, 128, &lv_font_montserrat_18);

  label(content, "LATEST", 148, 37, 128, &lv_font_montserrat_14);
  auto latest_value =
      label(content, ota_latest[0] ? ota_latest : "--", 148, 55, 128,
            &lv_font_montserrat_18);

  label(content, "STATUS", 288, 37, 136, &lv_font_montserrat_14);
  auto status_value =
      label(content, status_text(), 288, 55, 136, &lv_font_montserrat_16);

  lv_obj_set_style_text_color(current_value, lv_color_hex(TEXT), 0);
  lv_obj_set_style_text_color(latest_value, lv_color_hex(TEXT), 0);
  if (ota_view == OtaView::Current || ota_view == OtaView::Available)
    lv_obj_set_style_text_color(status_value, lv_color_hex(ACCENT), 0);
  else if (ota_view == OtaView::Error || ota_view == OtaView::Stale)
    lv_obj_set_style_text_color(status_value, lv_color_hex(WARNING), 0);

  // Channel and automatic-install preference share one row.
  label(content, "Channel", 8, 91, 72, &lv_font_montserrat_14);
  ota_channel_dropdown = lv_dropdown_create(content);
  lv_obj_set_pos(ota_channel_dropdown, 78, 82);
  lv_obj_set_size(ota_channel_dropdown, 205, 38);
  touchscreen_text::dropdown_set_options(ota_channel_dropdown,
                          "Production\nBeta\nDevelopment");
  lv_dropdown_set_selected(ota_channel_dropdown,
                           selection_from_channel(preferences.channel));
  lv_obj_add_event_cb(ota_channel_dropdown, channel_changed,
                      LV_EVENT_VALUE_CHANGED, nullptr);

  label(content, "Auto install", 294, 91, 80, &lv_font_montserrat_14);
  ota_auto_install_switch = lv_switch_create(content);
  lv_obj_set_pos(ota_auto_install_switch, 374, 85);
  lv_obj_set_size(ota_auto_install_switch, 58, 30);
  if (preferences.auto_install)
    lv_obj_add_state(ota_auto_install_switch, LV_STATE_CHECKED);

  ota_warning_label =
      label(content, "", 8, 127, 424, &lv_font_montserrat_14);
  lv_obj_set_height(ota_warning_label, 38);
  update_channel_warning();

  label(content, "Auto checks: after Wi-Fi starts, then every 24 hours.",
        8, 165, 424, &lv_font_montserrat_14);

  button(content, "Save settings", 8, 188, 205, save_update_settings);
  button(content, "Check now", 227, 188, 205, check_update);

  if (ota_view == OtaView::Available) {
    char install_text[64];
    snprintf(install_text, sizeof(install_text), "Install %s",
             ota_latest[0] ? ota_latest : "update");
    button(content, install_text, 8, 240, 424, install_update);
  }

  // Keep the operational details visible even when an install button appears.
  char last_check[48];
  format_last_check(last_check, sizeof(last_check));
  label(content, "Last check", 8, 291, 88, &lv_font_montserrat_14);
  ota_last_check_value =
      label(content, last_check, 98, 291, 128, &lv_font_montserrat_14);

  char footer[192];
  snprintf(footer, sizeof(footer),
           "Scale %s  |  %s  |  Protocol 1/1",
           current.firmware[0] ? current.firmware : "unknown",
           channel_display_name(preferences.channel));
  label(content, footer, 8, 312, 424, &lv_font_montserrat_14);
}
void update_page_watch(lv_timer_t *) {
  if (wifi_status_label && touchscreen_wifi_connected() != wifi_status_shown)
    render_wifi_status();
  if (ota_overlay)
    return;
  if (page_id != 4) {
    update_page_customized = false;
    ota_channel_dropdown = nullptr;
    ota_auto_install_switch = nullptr;
    ota_warning_label = nullptr;
    ota_last_check_value = nullptr;
    return;
  }
  if (!update_page_customized) {
    render_update_page();
    return;
  }
  if (ota_last_check_value) {
    char last_check[48];
    format_last_check(last_check, sizeof(last_check));
    touchscreen_text::label_set_text(ota_last_check_value, last_check);
  }
}

} // namespace

void touchscreen_ui_clear_notice_locked() {
  clear_notice_locked();
}

void touchscreen_home_set_update_available(bool available) {
  touchscreen_update_available_ui = available;
  refresh_menu_update_indicator();
}

// Called from the pairing timeout overlay while already in LVGL UI context.
// Clear the legacy pairing state and redraw Setup underneath the overlay.
void touchscreen_pairing_timeout_cleanup() {
  pair_code[0] = 0;
  if (page_id == PAGE_SETUP || page_id == PAGE_SCALES)
    build(page_id);
}

void ui_start(const Settings &s) {
  initial = s;
  // Use driver-owned frames and wait for bounce-frame completion before reuse.
  static_assert(CONFIG_BSP_LCD_RGB_BUFFER_NUMS >= 2,
                "Set BSP_LCD_RGB_BUFFER_NUMS=2 in menuconfig (Board Support Package)");
  lvgl_port_cfg_t port = ESP_LVGL_PORT_INIT_CONFIG();
  // The 7 KB default overflowed on Home once the -O2 build inlined LVGL's
  // draw path (V1.4.28 test build). Keep generous headroom.
  port.task_stack = 16384;
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
  auto root = touchscreen_ui_root();
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

  // Persistent connection status now uses the same dark-blue visual language
  // as the surrounding navigation controls. A colored dot carries state,
  // avoiding the old solid-green action-button appearance.
  connection_badge = lv_obj_create(root);
  lv_obj_set_pos(connection_badge, 188, 432);
  lv_obj_set_size(connection_badge, 118, 40);
  lv_obj_remove_flag(connection_badge, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_style_bg_color(connection_badge, lv_color_hex(0x173247), 0);
  lv_obj_set_style_bg_opa(connection_badge, LV_OPA_COVER, 0);
  lv_obj_set_style_border_color(connection_badge, lv_color_hex(0xc76a6a), 0);
  lv_obj_set_style_border_width(connection_badge, 1, 0);
  lv_obj_set_style_radius(connection_badge, 12, 0);
  lv_obj_set_style_pad_all(connection_badge, 0, 0);

  connection_badge_dot = lv_obj_create(connection_badge);
  lv_obj_set_pos(connection_badge_dot, 10, 15);
  lv_obj_set_size(connection_badge_dot, 10, 10);
  lv_obj_remove_flag(connection_badge_dot, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_remove_flag(connection_badge_dot, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_style_bg_color(connection_badge_dot, lv_color_hex(0xc76a6a), 0);
  lv_obj_set_style_bg_opa(connection_badge_dot, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(connection_badge_dot, 0, 0);
  lv_obj_set_style_radius(connection_badge_dot, 10, 0);
  lv_obj_set_style_pad_all(connection_badge_dot, 0, 0);

  connection_badge_text =
      label(connection_badge, "Offline", 26, 10, 84, &lv_font_montserrat_14);
  lv_obj_set_style_text_color(connection_badge_text, lv_color_hex(TEXT), 0);
  lv_obj_set_style_text_align(connection_badge_text, LV_TEXT_ALIGN_CENTER, 0);

  // Keep only Home / Switch Scale on the main screen. All management pages
  // live in the right-side drawer.
  home_nav_button = button(root, "Home", 12, 432, 164, nav,
                           reinterpret_cast<void *>(static_cast<intptr_t>(0)));
  lv_obj_set_size(home_nav_button, 164, 40);
  lv_obj_set_style_bg_color(home_nav_button, lv_color_hex(0x173b55), 0);
  lv_obj_set_style_bg_opa(home_nav_button, LV_OPA_COVER, 0);
  lv_obj_set_style_border_color(home_nav_button, lv_color_hex(0x55b7e8), 0);
  lv_obj_set_style_border_width(home_nav_button, 1, 0);
  lv_obj_set_style_radius(home_nav_button, 12, 0);
  if (lv_obj_t *home_text = lv_obj_get_child(home_nav_button, 0)) {
    lv_obj_set_style_text_color(home_text, lv_color_hex(TEXT), 0);
    lv_obj_set_style_text_font(home_text, &lv_font_montserrat_14, 0);
    lv_obj_set_height(home_text, 20);
  }
  ensure_scale_menu(root);
  ui_initialized = true;
  build(s.ssid[0] && scale_hosts_ui[active_scale_ui][0] ? 0 : 3);
  bsp_display_unlock();
  lv_timer_create(update_page_watch, 1000, nullptr);
}
void ui_state(const State &s) {
  if (!bsp_display_lock(1000))
    return;
  bool lost_connection = current.online && !s.online;
  const bool online_changed = current.online != s.online;
  if (!s.online) {
    editor_valid = false;
    cal_step = 0;
  }
  current = s;
  update_home_nav_button();
  if (connection_badge) {
    if (connection_badge_text)
      touchscreen_text::label_set_text(
          connection_badge_text, current.online ? "Connected" : "Offline");
    lv_obj_set_style_bg_color(connection_badge, lv_color_hex(0x173247), 0);
    lv_obj_set_style_border_color(
        connection_badge, lv_color_hex(0x55b7e8), 0);
    if (connection_badge_dot)
      lv_obj_set_style_bg_color(
          connection_badge_dot,
          lv_color_hex(current.online ? 0x42d3a2 : 0xc76a6a), 0);
  }
  if (lost_connection && (page_id == 0 || page_id == 1 || page_id == 2))
    build(page_id);
  else if (online_changed &&
           (page_id == PAGE_SETUP || page_id == PAGE_SCALES) && !keyboard)
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

void ui_message_for_generation(const char *s, uint32_t generation) {
  if (!bsp_display_lock(1000))
    return;
  if (!touchscreen_ui_generation_accepts(&content_generation, generation)) {
    ESP_LOGI("display",
             "Ignoring stale UI message generation=%lu current=%lu page=%d",
             (unsigned long)generation,
             (unsigned long)touchscreen_ui_generation_current(&content_generation),
             page_id);
    bsp_display_unlock();
    return;
  }
  message(s ? s : "");
  bsp_display_unlock();
}
void ui_pair_code(const char *code) {
  if (!bsp_display_lock(1000))
    return;
  snprintf(pair_code, sizeof(pair_code), "%s", code);
  build(PAGE_SCALES);
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
    transient_message(!strcmp(op, "save") ? "Keg information saved on scale"
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
  if (page_id == PAGE_SCALE_CONNECT && scale_list)
    fill_scale_list(discovered_scale_options);
  message("Tap a scale, or type its address below.");
  bsp_display_unlock();
}

void ui_discovered_options_for_generation(const char *options,
                                          uint32_t generation) {
  if (!bsp_display_lock(1000))
    return;
  if (!touchscreen_ui_generation_accepts(&content_generation, generation) ||
      page_id != PAGE_SCALE_CONNECT || !scale_list) {
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
  fill_scale_list(discovered_scale_options);
  bsp_display_unlock();
}

void ui_networks(const char *options, uint32_t generation) {
  if (!bsp_display_lock(1000))
    return;
  if (!touchscreen_ui_generation_accepts(&content_generation, generation) ||
      page_id != PAGE_WIFI || !networks) {
    ESP_LOGI("display",
             "Ignoring stale Wi-Fi scan result generation=%lu current=%lu page=%d",
             (unsigned long)generation,
             (unsigned long)touchscreen_ui_generation_current(&content_generation),
             page_id);
    bsp_display_unlock();
    return;
  }
  fill_network_list(options);
  bsp_display_unlock();
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

  if (!ui_initialized)
    return;
  if (!bsp_display_lock(1000))
    return;
  update_home_nav_button();
  if (page_id == PAGE_SETUP || page_id == PAGE_SCALES)
    build(page_id);
  bsp_display_unlock();
}

void ui_settings_applied(const Settings &settings) {
  if (!bsp_display_lock(1000))
    return;
  initial = settings;
  // After a save, leave the edit page for the overview that shows the result.
  if (page_id == PAGE_WIFI)
    build(PAGE_SETUP);
  else if (page_id == PAGE_SCALE_CONNECT)
    build(PAGE_SCALES);
  else if (page_id == PAGE_SETUP || page_id == PAGE_SCALES)
    build(page_id);
  bsp_display_unlock();
}

void ui_update_checking(void) {
  if (!bsp_display_lock(1000))
    return;
  ota_view = OtaView::Checking;
  ota_error_text[0] = 0;
  if (page_id == 4)
    render_update_page();
  bsp_display_unlock();
}

void ui_update_status(const char *current_version, const char *latest_version,
                      bool update_available, bool feed_stale) {
  if (!bsp_display_lock(1000))
    return;
  copy_text(ota_current, sizeof(ota_current), current_version);
  copy_text(ota_latest, sizeof(ota_latest), latest_version);
  ota_error_text[0] = 0;
  ota_view = feed_stale ? OtaView::Stale
                        : (update_available ? OtaView::Available
                                            : OtaView::Current);
  touchscreen_home_set_update_available(update_available && !feed_stale);
  if (page_id == 4 && !ota_overlay)
    render_update_page();
  bsp_display_unlock();
}

void ui_update_installing(const char *version) {
  if (!bsp_display_lock(1000))
    return;
  create_install_overlay(version);
  bsp_display_unlock();
}

void ui_update_progress(int percent) {
  if (percent < 0)
    percent = 0;
  if (percent > 100)
    percent = 100;
  if (!bsp_display_lock(1000))
    return;
  if (!ota_overlay)
    create_install_overlay(ota_latest);
  if (ota_overlay_percent)
    touchscreen_text::label_set_text_fmt(ota_overlay_percent, "%d%%", percent);
  if (ota_overlay_bar)
    lv_bar_set_value(ota_overlay_bar, percent, LV_ANIM_OFF);
  if (ota_overlay_status)
    touchscreen_text::label_set_text(ota_overlay_status,
                      percent < 100 ? "Downloading and verifying firmware..."
                                    : "Finalizing update...");
  bsp_display_unlock();
}

void ui_update_complete(const char *version) {
  if (!bsp_display_lock(1000))
    return;
  if (!ota_overlay)
    create_install_overlay(version);
  if (ota_overlay_title)
    touchscreen_text::label_set_text(ota_overlay_title, "Update Complete");
  if (ota_overlay_percent)
    touchscreen_text::label_set_text(ota_overlay_percent, "100%");
  if (ota_overlay_bar)
    lv_bar_set_value(ota_overlay_bar, 100, LV_ANIM_OFF);
  if (ota_overlay_status)
    touchscreen_text::label_set_text(ota_overlay_status,
                      "Firmware verified. Restarting touchscreen...");
  bsp_display_unlock();
}

void ui_update_error(const char *error, bool installing) {
  if (!bsp_display_lock(1000))
    return;
  copy_text(ota_error_text, sizeof(ota_error_text),
            error && error[0] ? error : "The update could not be completed.");

  if (installing || ota_overlay) {
    if (!ota_overlay)
      create_install_overlay(ota_latest);
    lv_obj_clean(ota_overlay);
    ota_overlay_title =
        label(ota_overlay, "Update Failed", 24, 68, 432,
              &lv_font_montserrat_24);
    label(ota_overlay, ota_error_text, 36, 145, 408, &lv_font_montserrat_18);
    label(ota_overlay,
          "No firmware change was activated. Check Wi-Fi and try again.",
          36, 230, 408, &lv_font_montserrat_16);
    button(ota_overlay, "Back to Firmware", 36, 340, 408, close_ota_overlay);
  } else {
    ota_view = OtaView::Error;
    if (page_id == 4)
      render_update_page();
  }
  bsp_display_unlock();
}
