idf_component_register(SRCS "main.c" "pixel_ui.c" "wifi_link.c" INCLUDE_DIRS "."
    REQUIRES esp_app_format controller_link nvs_flash esp_driver_gpio esp_timer led_strip
        esp_wifi esp_netif esp_event esp_http_server esp_websocket_client cjson mdns bootloader_support
    EMBED_TXTFILES "setup.html")
