idf_component_register(
    SRCS "ble_client.c"
    INCLUDE_DIRS "include"
    REQUIRES
        bt
        esp_app_format
)
