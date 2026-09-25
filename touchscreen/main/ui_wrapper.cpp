#include "lvgl.h"
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace touchscreen_text {
void append(char *dest, size_t size, size_t &used, const char *text) {
  if (!dest || !size || !text)
    return;
  while (*text && used + 1 < size)
    dest[used++] = *text++;
}

void sanitize(const char *source, char *dest, size_t size) {
  if (!dest || !size)
    return;
  dest[0] = 0;
  if (!source)
    return;

  size_t used = 0;
  for (size_t i = 0; source[i] && used + 1 < size;) {
    const unsigned char c0 = (unsigned char)source[i];
    if (c0 < 0x80) {
      dest[used++] = (char)c0;
      ++i;
      continue;
    }

    const unsigned char c1 = (unsigned char)source[i + 1];
    const unsigned char c2 = c1 ? (unsigned char)source[i + 2] : 0;
    const char *replacement = nullptr;
    size_t consumed = 1;

    if (c0 == 0xC2 && c1) {
      consumed = 2;
      switch (c1) {
      case 0xA0: replacement = " "; break;       // non-breaking space
      case 0xB0: replacement = " deg"; break;    // degree
      case 0xB1: replacement = "+/-"; break;     // plus/minus
      case 0xB7: replacement = "-"; break;       // middle dot
      default: break;
      }
    } else if (c0 == 0xC3 && c1) {
      consumed = 2;
      if (c1 >= 0x80 && c1 <= 0x85) replacement = "A";
      else if (c1 == 0x86) replacement = "AE";
      else if (c1 == 0x87) replacement = "C";
      else if (c1 >= 0x88 && c1 <= 0x8B) replacement = "E";
      else if (c1 >= 0x8C && c1 <= 0x8F) replacement = "I";
      else if (c1 == 0x91) replacement = "N";
      else if ((c1 >= 0x92 && c1 <= 0x96) || c1 == 0x98) replacement = "O";
      else if (c1 >= 0x99 && c1 <= 0x9C) replacement = "U";
      else if (c1 == 0x9D) replacement = "Y";
      else if (c1 == 0x9F) replacement = "ss";
      else if (c1 >= 0xA0 && c1 <= 0xA5) replacement = "a";
      else if (c1 == 0xA6) replacement = "ae";
      else if (c1 == 0xA7) replacement = "c";
      else if (c1 >= 0xA8 && c1 <= 0xAB) replacement = "e";
      else if (c1 >= 0xAC && c1 <= 0xAF) replacement = "i";
      else if (c1 == 0xB1) replacement = "n";
      else if ((c1 >= 0xB2 && c1 <= 0xB6) || c1 == 0xB8) replacement = "o";
      else if (c1 >= 0xB9 && c1 <= 0xBC) replacement = "u";
      else if (c1 == 0xBD || c1 == 0xBF) replacement = "y";
    } else if (c0 == 0xC5 && c1) {
      consumed = 2;
      if (c1 == 0x92) replacement = "OE";
      else if (c1 == 0x93) replacement = "oe";
    } else if (c0 == 0xE2 && c1 && c2) {
      consumed = 3;
      if (c1 == 0x80) {
        if (c2 >= 0x90 && c2 <= 0x94) replacement = "-";
        else if (c2 == 0x98 || c2 == 0x99) replacement = "'";
        else if (c2 == 0x9C || c2 == 0x9D) replacement = "\"";
        else if (c2 == 0xA2) replacement = "-";
        else if (c2 == 0xA6) replacement = "...";
      } else if (c1 == 0x86) {
        if (c2 == 0x90) replacement = "<-";
        else if (c2 == 0x92) replacement = "->";
      } else if (c1 == 0x9A && c2 == 0xA0) {
        replacement = "!";
      } else if (c1 == 0x9C && c2 == 0x93) {
        replacement = "OK";
      }
    } else if ((c0 & 0xF8) == 0xF0) {
      consumed = 4;
    } else if ((c0 & 0xF0) == 0xE0) {
      consumed = 3;
    } else if ((c0 & 0xE0) == 0xC0) {
      consumed = 2;
    }

    append(dest, size, used, replacement ? replacement : "?");
    i += consumed;
  }
  dest[used] = 0;
}

void label_set_text(lv_obj_t *object, const char *text) {
  char clean[512];
  sanitize(text, clean, sizeof(clean));
  lv_label_set_text(object, clean);
}

void label_set_text_fmt(lv_obj_t *object, const char *format, ...) {
  char formatted[512];
  va_list args;
  va_start(args, format);
  vsnprintf(formatted, sizeof(formatted), format, args);
  va_end(args);
  label_set_text(object, formatted);
}

void textarea_set_text(lv_obj_t *object, const char *text) {
  char clean[256];
  sanitize(text, clean, sizeof(clean));
  lv_textarea_set_text(object, clean);
}

void dropdown_set_options(lv_obj_t *object, const char *options) {
  const size_t source_len = options ? strlen(options) : 0;
  const size_t clean_size = source_len * 4 + 1;
  char *clean = clean_size > 1 ? (char *)malloc(clean_size) : nullptr;
  if (clean) {
    sanitize(options, clean, clean_size);
    lv_dropdown_set_options(object, clean);
    free(clean);
    return;
  }
  char fallback[256];
  sanitize(options, fallback, sizeof(fallback));
  lv_dropdown_set_options(object, fallback);
}
} // namespace touchscreen_text

#define lv_label_set_text touchscreen_text::label_set_text
#define lv_label_set_text_fmt touchscreen_text::label_set_text_fmt
#define lv_textarea_set_text touchscreen_text::textarea_set_text
#define lv_dropdown_set_options touchscreen_text::dropdown_set_options
#define ui_start ui_start_legacy
#define ui_update_progress ui_update_progress_legacy
#include "ui.cpp"
#undef ui_update_progress
#undef ui_start

namespace {
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
    lv_label_set_text(
        ota_warning_label,
        "Stable releases. Recommended for normal use.");
    lv_obj_set_style_text_color(ota_warning_label, lv_color_hex(ACCENT), 0);
  } else if (selected == 1) {
    lv_label_set_text(
        ota_warning_label,
        "Pre-release validation builds. May contain unfinished changes.");
    lv_obj_set_style_text_color(ota_warning_label, lv_color_hex(WARNING), 0);
  } else {
    lv_label_set_text(
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

  ota_overlay = lv_obj_create(lv_screen_active());
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
    return "Feed syncing";
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
  lv_dropdown_set_options(ota_channel_dropdown,
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
    lv_label_set_text(ota_last_check_value, last_check);
  }
}
} // namespace

void touchscreen_update_page_render_now() { render_update_page(); }

void ui_start(const Settings &settings) {
  ui_start_legacy(settings);
  lv_timer_create(update_page_watch, 1000, nullptr);
}

void ui_settings_applied(const Settings &settings) {
  if (!bsp_display_lock(1000))
    return;
  initial = settings;
  if (page_id == 3)
    build(3);
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
    lv_label_set_text_fmt(ota_overlay_percent, "%d%%", percent);
  if (ota_overlay_bar)
    lv_bar_set_value(ota_overlay_bar, percent, LV_ANIM_OFF);
  if (ota_overlay_status)
    lv_label_set_text(ota_overlay_status,
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
    lv_label_set_text(ota_overlay_title, "Update Complete");
  if (ota_overlay_percent)
    lv_label_set_text(ota_overlay_percent, "100%");
  if (ota_overlay_bar)
    lv_bar_set_value(ota_overlay_bar, 100, LV_ANIM_OFF);
  if (ota_overlay_status)
    lv_label_set_text(ota_overlay_status,
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