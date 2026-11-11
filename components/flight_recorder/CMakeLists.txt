idf_component_register(SRCS "flight_recorder.c" INCLUDE_DIRS "include"
    REQUIRES esp_timer esp_system heap esp_wifi freertos)
