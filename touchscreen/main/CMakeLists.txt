idf_component_register(SRCS "main_wrapper.cpp" "app_task.cpp" "ui.cpp" "touchscreen_ota.cpp" INCLUDE_DIRS "."
    REQUIRES esp_lcd_touch esp_lvgl_port controller_link esp_wifi esp_netif esp_event nvs_flash esp_timer app_update esp_https_ota esp_http_client esp_websocket_client cjson mdns esp32_s3_touch_lcd_4b lvgl)

# main_wrapper.cpp includes main.cpp after redirecting only the settings-save
# restart to live Wi-Fi/scale reconfiguration. OTA keeps the real esp_restart().
