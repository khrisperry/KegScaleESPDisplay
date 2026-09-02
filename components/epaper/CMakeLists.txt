idf_component_register(
    SRCS
        "epaper.c"
        "epaper_fonts.c"
    INCLUDE_DIRS "include"
    REQUIRES
        esp_driver_gpio
        esp_driver_spi
)
