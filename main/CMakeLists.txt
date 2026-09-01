idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES
        ble_client
        display_ui
        pairing
        touch_wake
        nvs_flash
        esp_driver_gpio
        esp_hw_support
)
