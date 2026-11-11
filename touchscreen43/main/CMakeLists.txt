set(shared "${CMAKE_CURRENT_LIST_DIR}/../../touchscreen/main")
set(sources main.cpp connection_transport.cpp connection_session.cpp
    controller_setup_client.cpp setup_discovery.cpp ui.cpp ui_text.cpp
    home_layout.cpp vessel_artwork.cpp pairing_overlay.cpp pairing_guard.cpp)
list(TRANSFORM sources PREPEND "${shared}/")
idf_component_register(SRCS ${sources} "app_task.cpp" "bsp43.c" "ota_disabled.cpp"
    INCLUDE_DIRS "." "${shared}"
    REQUIRES flight_recorder esp_lcd esp_lcd_touch esp_lcd_touch_gt911 esp_lvgl_port
      esp_driver_gpio esp_driver_i2c controller_link ble_client esp_wifi
      esp_netif esp_event nvs_flash esp_timer app_update esp_https_ota
      esp_http_client esp_websocket_client cjson mbedtls mdns lvgl)
target_compile_definitions(${COMPONENT_LIB} PUBLIC KEG_TOUCHSCREEN_43=1
    CONFIG_BSP_LCD_RGB_BUFFER_NUMS=2 CONFIG_BSP_LCD_RGB_BOUNCE_BUFFER_HEIGHT=20)
set_source_files_properties("${shared}/main.cpp" PROPERTIES COMPILE_DEFINITIONS
    "ui_start=touchscreen_ui_start_dispatch;ui_state=touchscreen_ui_state_dispatch;ui_pair_code=touchscreen_ui_pair_code_dispatch;ui_paired=touchscreen_ui_paired_dispatch")
if(NOT CMAKE_SCRIPT_MODE_FILE)
    idf_build_get_property(python PYTHON)
    set(vessel_images "${CMAKE_CURRENT_BINARY_DIR}/vessel_images.cpp")
    add_custom_command(OUTPUT "${vessel_images}"
      COMMAND "${python}" "${shared}/../../tools/generate_vessel_artwork.py"
        "${shared}/artwork/web_vessels.json" "${vessel_images}"
      DEPENDS "${shared}/artwork/web_vessels.json"
        "${shared}/../../tools/generate_vessel_artwork.py" VERBATIM)
    target_sources(${COMPONENT_LIB} PRIVATE "${vessel_images}")
endif()
