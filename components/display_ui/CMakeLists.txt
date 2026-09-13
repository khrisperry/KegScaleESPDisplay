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

# Keep the existing generic message renderer available for non-calibration
# status screens. calibration_message.c provides the public wrapper and uses
# the original implementation as its fallback.
set_source_files_properties(
    display_ui.c
    PROPERTIES
        COMPILE_DEFINITIONS
            "display_ui_show_message=display_ui_show_message_original"
)
