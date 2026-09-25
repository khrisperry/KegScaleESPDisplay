idf_component_register(SRCS "main.cpp" "app_task.cpp" "connection_transport.cpp" "setup_discovery.cpp" "ui.cpp" "ui_text.cpp" "home_layout.cpp" "touchscreen_ota.cpp" "pairing_overlay.cpp" "pairing_guard.cpp" INCLUDE_DIRS "."
    REQUIRES esp_lcd_touch esp_lvgl_port controller_link ble_client esp_wifi esp_netif esp_event nvs_flash esp_timer app_update esp_https_ota esp_http_client esp_websocket_client cjson mdns esp32_s3_touch_lcd_4b lvgl)

# main.cpp now owns live settings, pairing synchronization, discovery and OTA
# scheduling directly. Route only its UI entry points through home_layout.cpp
# and pairing_overlay.cpp; there is no source-inclusion or macro-interception
# application wrapper.
set_source_files_properties("main.cpp" PROPERTIES COMPILE_DEFINITIONS
    "ui_start=touchscreen_ui_start_dispatch;ui_state=touchscreen_ui_state_dispatch;ui_pair_code=touchscreen_ui_pair_code_dispatch;ui_paired=touchscreen_ui_paired_dispatch")

# ui.cpp and ui_text.cpp are compiled directly. Touch UI behavior must not rely
# on source inclusion or LVGL macro interception.
