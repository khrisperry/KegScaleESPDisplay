# Run before project() loads Kconfig. Existing sdkconfig choices override
# sdkconfig.defaults, so migrate the old fixed pool on an ordinary local build.
# Only allocator-choice lines change; other local settings remain intact.
if(DEFINED SDKCONFIG AND NOT "${SDKCONFIG}" STREQUAL "")
    get_filename_component(_touch_lvgl_sdkconfig "${SDKCONFIG}" ABSOLUTE
                           BASE_DIR "${CMAKE_CURRENT_LIST_DIR}")
else()
    set(_touch_lvgl_sdkconfig "${CMAKE_CURRENT_LIST_DIR}/sdkconfig")
endif()

if(EXISTS "${_touch_lvgl_sdkconfig}")
    file(READ "${_touch_lvgl_sdkconfig}" _touch_lvgl_config)
    if(_touch_lvgl_config MATCHES "(^|\n)CONFIG_LV_USE_BUILTIN_MALLOC=y(\r?\n|$)")
        string(REPLACE "CONFIG_LV_USE_BUILTIN_MALLOC=y"
                       "# CONFIG_LV_USE_BUILTIN_MALLOC is not set"
                       _touch_lvgl_config "${_touch_lvgl_config}")
        if(_touch_lvgl_config MATCHES "# CONFIG_LV_USE_CLIB_MALLOC is not set")
            string(REPLACE "# CONFIG_LV_USE_CLIB_MALLOC is not set"
                           "CONFIG_LV_USE_CLIB_MALLOC=y"
                           _touch_lvgl_config "${_touch_lvgl_config}")
        elseif(NOT _touch_lvgl_config MATCHES "(^|\n)CONFIG_LV_USE_CLIB_MALLOC=y(\r?\n|$)")
            string(APPEND _touch_lvgl_config "\nCONFIG_LV_USE_CLIB_MALLOC=y\n")
        endif()
        file(WRITE "${_touch_lvgl_sdkconfig}" "${_touch_lvgl_config}")
        message(STATUS "Touchscreen: migrated LVGL fixed pool to system malloc (PSRAM eligible)")
    endif()
endif()
unset(_touch_lvgl_sdkconfig)
unset(_touch_lvgl_config)
