idf_component_register(SRCS "main_wrapper.cpp" "app_task.cpp" "ui_wrapper.cpp" "home_layout.cpp" "touchscreen_ota.cpp" "pairing_overlay.cpp" "pairing_guard.cpp" INCLUDE_DIRS "."
    REQUIRES esp_lcd_touch esp_lvgl_port controller_link esp_wifi esp_netif esp_event nvs_flash esp_timer app_update esp_https_ota esp_http_client esp_websocket_client cjson mdns esp32_s3_touch_lcd_4b lvgl)

# main_wrapper.cpp includes main.cpp after redirecting only the settings-save
# restart to live Wi-Fi/scale reconfiguration. Route its UI entry points through
# home_layout.cpp so Dashboard can be the default home screen with an optional
# persistent Glass view while keeping the legacy UI/state model intact. Pairing
# callbacks are routed through pairing_overlay.cpp so the authorization code
# temporarily owns the entire touchscreen.
set_source_files_properties("main_wrapper.cpp" PROPERTIES COMPILE_DEFINITIONS
    "ui_start=touchscreen_ui_start_dispatch;ui_state=touchscreen_ui_state_dispatch;ui_pair_code=touchscreen_ui_pair_code_dispatch;ui_paired=touchscreen_ui_paired_dispatch")

# ui_wrapper.cpp includes ui.cpp and exposes an in-place Settings refresh.
# OTA keeps the real esp_restart().
