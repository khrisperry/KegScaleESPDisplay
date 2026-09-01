idf_component_register(
    SRCS "pairing.c"
    INCLUDE_DIRS "include"
    REQUIRES
        nvs_flash
        ble_client
)
