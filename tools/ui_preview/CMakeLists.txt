# Off-host UI preview: compiles the SAME vendored LVGL the firmware uses
# (firmware/esp32/managed_components/lvgl__lvgl) for the host and renders
# WaveX screens to BMP files in out/. See preview.c for what and why.
#
#   cmake -B build && cmake --build build -j && (mkdir -p out && ./build/ui_preview)

cmake_minimum_required(VERSION 3.16)
project(wavex-ui-preview C)

set(LVGL_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../firmware/esp32/managed_components/lvgl__lvgl)

file(GLOB_RECURSE LVGL_SOURCES ${LVGL_DIR}/src/*.c)

add_executable(ui_preview preview.c ${LVGL_SOURCES})

target_include_directories(ui_preview PRIVATE
    ${LVGL_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}
)

# Point LVGL at our lv_conf.h (mirrors the device's visual configuration).
target_compile_definitions(ui_preview PRIVATE
    LV_CONF_PATH="${CMAKE_CURRENT_SOURCE_DIR}/lv_conf.h"
)

# Vendored third-party code; its warnings are not ours to fix.
target_compile_options(ui_preview PRIVATE -w -O2)
target_link_libraries(ui_preview m)
