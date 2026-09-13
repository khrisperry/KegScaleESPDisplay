idf_component_register(SRCS "main.cpp" "ui.cpp" "touchscreen_ota.cpp" INCLUDE_DIRS "."
    REQUIRES controller_link esp_wifi esp_netif esp_event nvs_flash esp_timer app_update esp_https_ota esp_http_client esp_websocket_client cjson mdns esp32_s3_touch_lcd_4b lvgl)
