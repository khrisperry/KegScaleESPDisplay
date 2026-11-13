idf_component_register(SRCS "flight_recorder.c" INCLUDE_DIRS "include"
    REQUIRES xtensa esp_timer esp_system heap esp_wifi freertos)
