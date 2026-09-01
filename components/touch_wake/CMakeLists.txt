idf_component_register(
    SRCS "touch_wake.c"
    INCLUDE_DIRS "include"
    REQUIRES
        esp_driver_touch_sens
)
