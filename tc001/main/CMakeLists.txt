idf_component_register(SRCS "main.c" "pixel_ui.c" INCLUDE_DIRS "."
    REQUIRES ble_client pairing nvs_flash esp_driver_gpio esp_timer led_strip)
