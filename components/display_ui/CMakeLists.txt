idf_component_register(
    SRCS
        "display_ui.c"
        "calibration_message.c"
    INCLUDE_DIRS "include"
    REQUIRES
        epaper
        ble_client
        qrcode
)
