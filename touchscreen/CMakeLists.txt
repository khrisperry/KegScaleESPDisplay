cmake_minimum_required(VERSION 3.16)
set(EXTRA_COMPONENT_DIRS "../components/ble_client")
include("${CMAKE_CURRENT_LIST_DIR}/lvgl_memory.cmake")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(keg_scale_touchscreen)

# The default 64 KiB LVGL pool cannot reliably fit the full UI and masked
# image draw layers. Reject stale fixed-pool configs rather than ship a hang.
if(CONFIG_LV_USE_BUILTIN_MALLOC)
    message(FATAL_ERROR "Touchscreen LVGL must use system malloc. Reconfigure with CONFIG_LV_USE_CLIB_MALLOC=y.")
endif()
