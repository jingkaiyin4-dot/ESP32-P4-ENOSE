# Copyright (2025) Beijing Volcano Engine Technology Ltd.
# SPDX-License-Identifier: Apache-2.0

# ConversationalAI SDK ESP-IDF component configuration
# Converted from standard CMake to ESP-IDF component format

# Source files
set(VOLC_ESP_SRCS "${CMAKE_CURRENT_LIST_DIR}/../platform/src/espressif/volc_hal.c"
                    "${CMAKE_CURRENT_LIST_DIR}/../platform/src/espressif/volc_hal_button.c"
                    "${CMAKE_CURRENT_LIST_DIR}/../platform/src/espressif/volc_hal_capture.c"
                    "${CMAKE_CURRENT_LIST_DIR}/../platform/src/espressif/volc_hal_display.c"
                    "${CMAKE_CURRENT_LIST_DIR}/../platform/src/espressif/volc_hal_file.c"
                    "${CMAKE_CURRENT_LIST_DIR}/../platform/src/espressif/volc_hal_led.c"
                    "${CMAKE_CURRENT_LIST_DIR}/../platform/src/espressif/volc_hal_player.c"
                    "${CMAKE_CURRENT_LIST_DIR}/../platform/src/espressif/basic_board.c"
)
set(VOLC_ESP_DISPLAY_SRCS "${CMAKE_CURRENT_LIST_DIR}/../platform/src/espressif/lvgl_source/echoear_font.c"
                          "${CMAKE_CURRENT_LIST_DIR}/../platform/src/espressif/lvgl_source/img_app_aiPhone.c"
)
set(VOLC_APP_SRCS ${VOLC_FRAMEWORK_SRCS}
                  ${VOLC_SERVICE_SRCS}
                  ${VOLC_ESP_SRCS}
                  ${VOLC_ESP_DISPLAY_SRCS}    
)

set(VOLC_LVGL_SOURCE_INCS "${CMAKE_CURRENT_LIST_DIR}/../platform/src/espressif/lvgl_source/")
set(VOLC_ASSETS_SOURCE_INCS "${CMAKE_CURRENT_LIST_DIR}/../../examples/high_quality_solution/espressif/main/")


set(VOLC_APP_INCS ${VOLC_FRAMEWORK_INCS}
    ${VOLC_PLATFORM_INCS}
    ${VOLC_SERVICE_INCS}
    ${VOLC_UTIL_INCS}
    ${VOLC_LVGL_SOURCE_INCS}
    ${VOLC_ASSETS_SOURCE_INCS}
)

idf_component_register(
    SRCS ${VOLC_APP_SRCS}
    INCLUDE_DIRS ${VOLC_APP_INCS}
    REQUIRES volc_conv_ai
    PRIV_REQUIRES lvgl button av_processor esp_board_manager hw_init esp_mmap_assets esp_emote_gfx esp_vocat_base_control
)

# Compiler flags
target_compile_options(${COMPONENT_LIB} PRIVATE
    -w
    -Os
    -ffunction-sections
    -fdata-sections
)

# Export the library for use by other components
set(VOLC_CONV_APP_LIBRARIES ${COMPONENT_LIB} PARENT_SCOPE)
set(VOLC_CONV_APP_INCLUDE_DIRS ${CMAKE_CURRENT_SOURCE_DIR}/../framework/inc PARENT_SCOPE)
