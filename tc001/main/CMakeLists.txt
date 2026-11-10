idf_component_register(SRCS "main.c" "pixel_ui.c" "wifi_link.c" "mqtt_feed.c" "battery.c" INCLUDE_DIRS "."
    REQUIRES esp_app_format esp_adc nvs_flash esp_driver_gpio esp_driver_spi esp_timer led_strip
        esp_wifi esp_netif esp_event esp_http_server mqtt cjson mdns bootloader_support mbedtls lwip
    EMBED_TXTFILES "setup.html" "battery.html")
