cmake_minimum_required(VERSION 3.16)
set(EXTRA_COMPONENT_DIRS "../components/flight_recorder" "../components/ble_client" "../touchscreen/components/controller_link")
set(SDKCONFIG_DEFAULTS "../touchscreen/sdkconfig.defaults;sdkconfig.defaults")
include("${CMAKE_CURRENT_LIST_DIR}/../touchscreen/lvgl_memory.cmake")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(keg_scale_touchscreen43)

# ESP-IDF NimBLE store trips a false -Warray-bounds at -O2 with
# BLE_STORE_MAX_BONDS=1: the [i+1] read sits behind a count>1 guard the
# compiler cannot see through. Keep it a warning for that component only.
idf_component_get_property(bt_lib bt COMPONENT_LIB)
target_compile_options(${bt_lib} PRIVATE -Wno-error=array-bounds)
