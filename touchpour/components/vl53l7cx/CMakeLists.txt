idf_component_register(SRCS "src/vl53l7cx_api.c" "src/vl53l7cx_buffers.c" "src/platform.c"
    INCLUDE_DIRS "include" REQUIRES esp_driver_i2c esp_driver_gpio freertos)
