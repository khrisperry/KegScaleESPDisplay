set(shared "../../touchscreen/main")
idf_component_register(SRCS "main.cpp" "${shared}/main.cpp" "board.cpp" "ui.cpp" "vessel_artwork.cpp"
    "${shared}/connection_transport.cpp" "${shared}/connection_session.cpp"
    "${shared}/controller_setup_client.cpp" "${shared}/setup_discovery.cpp"
    "${shared}/pairing_guard.cpp" "${shared}/ui_text.cpp"
    INCLUDE_DIRS "." "${shared}"
    REQUIRES esp_lvgl_port lvgl esp_lcd esp_driver_spi esp_driver_i2c esp_driver_ledc esp_driver_gpio
    tap_control controller_link ble_client esp_wifi esp_netif esp_event nvs_flash esp_timer
    app_update esp_http_client esp_websocket_client cjson mdns)

set_source_files_properties("${shared}/main.cpp" PROPERTIES COMPILE_DEFINITIONS "TOUCHPOUR_FIRMWARE")

# IDF also evaluates component requirements in CMake script mode.
if(NOT CMAKE_SCRIPT_MODE_FILE)
    # The checked-in compact artwork needs only IDF's Python standard library.
    idf_build_get_property(python PYTHON)
    set(vessel_pixels "${CMAKE_CURRENT_BINARY_DIR}/serving_pixels.cpp")
    add_custom_command(OUTPUT "${vessel_pixels}"
        COMMAND "${python}" "${CMAKE_CURRENT_LIST_DIR}/../tools/generate_vessel_artwork.py"
            "${CMAKE_CURRENT_LIST_DIR}/artwork/serving_vessels.json" "${vessel_pixels}"
        DEPENDS "${CMAKE_CURRENT_LIST_DIR}/artwork/serving_vessels.json"
            "${CMAKE_CURRENT_LIST_DIR}/../tools/generate_vessel_artwork.py"
        VERBATIM)
    target_sources(${COMPONENT_LIB} PRIVATE "${vessel_pixels}")
endif()
