#include "ui.cpp"

void ui_settings_applied(const Settings &settings) {
  if (!bsp_display_lock(1000))
    return;
  initial = settings;
  if (page_id == 3)
    build(3);
  bsp_display_unlock();
}
