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
lv_obj_t *ota_overlay = nullptr;
lv_obj_t *ota_overlay_title = nullptr;
lv_obj_t *ota_overlay_percent = nullptr;
lv_obj_t *ota_overlay_status = nullptr;
lv_obj_t *ota_overlay_bar = nullptr;

void copy_text(char *dest, size_t size, const char *source) {
  if (!dest || !size)
    return;
  snprintf(dest, size, "%s", source ? source : "");
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

void render_update_page();

void check_update(lv_event_t *) {
  ota_view = OtaView::Checking;
  render_update_page();
  touchscreen_set_ota_install_mode(false);
  if (!submit("ota", nullptr)) {
    ota_view = OtaView::Error;
    copy_text(ota_error_text, sizeof(ota_error_text),
              "Could not start the update check. Please try again.");
    render_update_page();
  }
}

void close_ota_overlay(lv_event_t *) {
  clear_ota_overlay();
  ota_view = OtaView::Idle;
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
      label(ota_overlay, "Preparing secure download…", 36, 285, 408,
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

void install_update(lv_event_t *) {
  ota_view = OtaView::Preparing;
  render_update_page();
  touchscreen_set_ota_install_mode(true);
  if (!submit("ota", nullptr)) {
    ota_view = OtaView::Error;
    copy_text(ota_error_text, sizeof(ota_error_text),
              "Could not start the firmware update. Please try again.");
    render_update_page();
  }
}

void render_update_page() {
  if (page_id != 4 || ota_overlay)
    return;

  update_page_customized = true;
  lv_obj_clean(content);
  headline = detail = connection = arc = networks = nullptr;
  memset(fields, 0, sizeof(fields));

  label(content, "Firmware Update", 8, 0, 424, &lv_font_montserrat_24);

  char versions[120];
  const char *running = esp_app_get_description()->version;
  snprintf(versions, sizeof(versions), "Current version   %s", running);
  label(content, versions, 8, 42, 424, &lv_font_montserrat_18);

  if (ota_view == OtaView::Idle) {
    label(content,
          "Check the development channel for touchscreen firmware. Checking "
          "only reads the published update information; it does not install "
          "firmware.",
          8, 88, 424, &lv_font_montserrat_16);
    button(content, "Check for update", 8, 220, 424, check_update);
  } else if (ota_view == OtaView::Checking) {
    label(content, "Checking for updates…", 8, 100, 424,
          &lv_font_montserrat_24);
    label(content,
          "Contacting the update service and validating the firmware manifest.",
          8, 150, 424, &lv_font_montserrat_16);
  } else if (ota_view == OtaView::Preparing) {
    label(content, "Preparing update…", 8, 100, 424,
          &lv_font_montserrat_24);
    label(content,
          "Revalidating the update before installation begins.",
          8, 150, 424, &lv_font_montserrat_16);
  } else if (ota_view == OtaView::Current) {
    label(content, "Your firmware is current", 8, 94, 424,
          &lv_font_montserrat_24);
    snprintf(versions, sizeof(versions), "Latest version    %s", ota_latest);
    label(content, versions, 8, 145, 424, &lv_font_montserrat_18);
    button(content, "Check again", 8, 230, 424, check_update);
  } else if (ota_view == OtaView::Available) {
    label(content, "Update available", 8, 86, 424, &lv_font_montserrat_24);
    snprintf(versions, sizeof(versions), "%s  →  %s", ota_current, ota_latest);
    auto versions_label =
        label(content, versions, 8, 135, 424, &lv_font_montserrat_20);
    lv_obj_set_style_text_color(versions_label, lv_color_hex(ACCENT), 0);
    label(content,
          "Keep the touchscreen connected to power. Installation will take "
          "over the display until the device restarts.",
          8, 178, 424, &lv_font_montserrat_16);
    char button_text[64];
    snprintf(button_text, sizeof(button_text), "Install %s", ota_latest);
    button(content, button_text, 8, 258, 424, install_update);
  } else if (ota_view == OtaView::Stale) {
    label(content, "Update service is syncing", 8, 88, 424,
          &lv_font_montserrat_24);
    snprintf(versions, sizeof(versions), "Published version  %s", ota_latest);
    label(content, versions, 8, 140, 424, &lv_font_montserrat_18);
    label(content,
          "The published firmware is older than this touchscreen, so it will "
          "not be installed. Try again after publishing finishes.",
          8, 180, 424, &lv_font_montserrat_16);
    button(content, "Check again", 8, 270, 424, check_update);
  } else {
    label(content, "Could not check for updates", 8, 88, 424,
          &lv_font_montserrat_24);
    label(content, ota_error_text, 8, 145, 424, &lv_font_montserrat_16);
    button(content, "Try again", 8, 250, 424, check_update);
  }

  char footer[96];
  snprintf(footer, sizeof(footer), "Scale firmware: %s   |   Wi-Fi protocol: 1",
           current.firmware[0] ? current.firmware : "unknown");
  label(content, footer, 8, 305, 424, &lv_font_montserrat_14);
}

void update_page_watch(lv_timer_t *) {
  if (ota_overlay)
    return;
  if (page_id != 4) {
    update_page_customized = false;
    return;
  }
  if (!update_page_customized)
    render_update_page();
}
} // namespace

void ui_start(const Settings &settings) {
  ui_start_legacy(settings);
  lv_timer_create(update_page_watch, 20, nullptr);
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
  if (page_id != 4)
    page_id = 4;
  render_update_page();
  bsp_display_unlock();
}

void ui_update_status(const char *current_version, const char *latest_version,
                      bool update_available, bool feed_stale) {
  if (!bsp_display_lock(1000))
    return;
  clear_ota_overlay();
  copy_text(ota_current, sizeof(ota_current), current_version);
  copy_text(ota_latest, sizeof(ota_latest), latest_version);
  ota_view = feed_stale ? OtaView::Stale
                        : (update_available ? OtaView::Available
                                            : OtaView::Current);
  if (page_id != 4)
    page_id = 4;
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
                      percent < 100 ? "Downloading and verifying firmware…"
                                    : "Finalizing update…");
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
                      "Firmware verified. Restarting touchscreen…");
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
    if (page_id != 4)
      page_id = 4;
    render_update_page();
  }
  bsp_display_unlock();
}
