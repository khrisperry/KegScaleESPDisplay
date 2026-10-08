idf_component_register(SRCS "wifi_settings.cpp" "app.cpp" "network.cpp" "foam_capture.cpp" "pour_history.cpp" INCLUDE_DIRS "."
    REQUIRES vl53l0x esp_driver_gpio esp_driver_ledc esp_timer nvs_flash esp_wifi esp_netif esp_http_server cjson lwip app_update esp_partition
    EMBED_TXTFILES "index.html")

idf_build_get_property(idf_python PYTHON)
set(gzip_page "${CMAKE_CURRENT_BINARY_DIR}/index.html.gz")
add_custom_command(OUTPUT "${gzip_page}"
    COMMAND "${idf_python}" "${CMAKE_CURRENT_SOURCE_DIR}/../../tools/compress_web.py"
        "${CMAKE_CURRENT_SOURCE_DIR}/index.html" "${gzip_page}"
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/index.html"
        "${CMAKE_CURRENT_SOURCE_DIR}/../../tools/compress_web.py"
    VERBATIM)
add_custom_target(tap_web_gzip DEPENDS "${gzip_page}")
target_add_binary_data(${COMPONENT_LIB} "${gzip_page}" BINARY DEPENDS tap_web_gzip)
