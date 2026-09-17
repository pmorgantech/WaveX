# Widget tests: the real vendored LVGL, rendered to memory, asserted by pixel.
#
# The parent test suite links `mocks/lvgl.h` - a ~100-line stub with no
# lv_area_t and no draw API, written for file_browser.cpp. Anything that
# actually DRAWS cannot compile against it, which is why UI widgets had no
# coverage and why the stacked L/R waveform change shipped untested.
#
# Here we link the SAME vendored LVGL the firmware ships. Its software renderer
# draws into a plain memory buffer with no display driver, no SDL and no
# hardware, so a widget's real output can be asserted on directly.
# tools/ui_preview has rendered design previews this way since it was written;
# the only thing missing was pointing a test at it. See docs/roadmap.md.

# THE important line. Directory-level include_directories() are INHERITED by
# subdirectories, and the parent puts `mocks/` first specifically to shadow
# lvgl.h. Inheriting that here compiles LVGL against a stub with no
# LV_VERSION_CHECK, which fails in the vendored font tables with an error that
# points nowhere near the cause. Clear the inherited list and state our own.
set_property(DIRECTORY PROPERTY INCLUDE_DIRECTORIES "")

set(LVGL_HOST_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../managed_components/lvgl__lvgl)

file(GLOB widget_tests "${CMAKE_CURRENT_SOURCE_DIR}/*_test.cpp")

if(NOT widget_tests)
    return()
endif()

if(NOT EXISTS ${LVGL_HOST_DIR}/lvgl.h)
    # Managed components are fetched by the ESP-IDF build. Skipping rather than
    # failing keeps `make test` working in a tree that has never built the
    # firmware; the tests are simply absent, which ctest reports honestly.
    message(STATUS "Widget tests skipped: vendored LVGL not present at ${LVGL_HOST_DIR}")
    return()
endif()

# The host LVGL configuration is SHARED with tools/ui_preview rather than
# copied. Two lv_conf.h files would drift, and a test rendering under different
# settings from the preview - or from the device - is worth less than no test.
set(LVGL_HOST_CONF ${CMAKE_CURRENT_SOURCE_DIR}/../../../../tools/ui_preview/lv_conf.h)

if(NOT EXISTS ${LVGL_HOST_CONF})
    message(FATAL_ERROR "Widget tests need ${LVGL_HOST_CONF}; it is the shared host LVGL config")
endif()

file(GLOB_RECURSE LVGL_HOST_SOURCES ${LVGL_HOST_DIR}/src/*.c)
add_library(lvgl_host STATIC ${LVGL_HOST_SOURCES})
# SYSTEM: LVGL's headers are pulled into our C++ sources too, and the parent's
# -Wconversion fires inside lv_draw_buf.h. Third-party warnings we cannot fix
# would drown the ones we can.
target_include_directories(lvgl_host SYSTEM PUBLIC ${LVGL_HOST_DIR})
target_compile_definitions(lvgl_host PUBLIC LV_CONF_PATH="${LVGL_HOST_CONF}")
# Vendored third-party code; its warnings are not ours to fix, and the parent's
# -Wconversion would bury the build in them. -O1 because a debug-speed software
# renderer makes these tests take seconds rather than milliseconds.
target_compile_options(lvgl_host PRIVATE -w -O1)

foreach(test_source IN LISTS widget_tests)
    cmake_path(GET test_source STEM test_name)

    add_executable(${test_name}
        ${test_source}
        ${CMAKE_CURRENT_SOURCE_DIR}/../../components/ui/components/waveform_view.cpp
    )

    target_include_directories(${test_name} PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../../components/ui/components
        # waveform_view.cpp takes its font from the type ladder like every
        # other UI source, so the theme header has to be reachable. It
        # contributes macros only - the mono tables it declares are not
        # referenced here, so nothing extra has to be linked.
        ${CMAKE_CURRENT_SOURCE_DIR}/../../components/ui/styles
        ${CMAKE_CURRENT_SOURCE_DIR}/../../components/ui/fonts
        ${CMAKE_CURRENT_SOURCE_DIR}/../../../shared
    )

    if(test_name STREQUAL "softkey_bar_test")
        target_sources(${test_name} PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/../../components/ui/src/ui_softkey_bar.cpp)
        target_include_directories(${test_name} PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/stubs
            ${CMAKE_CURRENT_SOURCE_DIR}/../../components/ui/include)
    endif()

    if(test_name STREQUAL "shift_touch_test")
        file(GLOB SHIFT_TOUCH_FONTS
            ${CMAKE_CURRENT_SOURCE_DIR}/../../components/ui/fonts/wavex_mono_*.c)
        target_sources(${test_name} PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/../../components/ui/src/ui_navigator.cpp
            ${CMAKE_CURRENT_SOURCE_DIR}/../../components/ui/src/ui_softkey_bar.cpp
            ${CMAKE_CURRENT_SOURCE_DIR}/../../components/ui/src/multi_touch_input.cpp
            ${SHIFT_TOUCH_FONTS})
        target_include_directories(${test_name} PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/stubs
            ${CMAKE_CURRENT_SOURCE_DIR}/../../components/ui/include)
    endif()

    if(test_name STREQUAL "parameter_widgets_test")
        target_include_directories(${test_name} PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/../../components/ui/include)
        file(GLOB PARAMETER_WIDGET_FONTS
            ${CMAKE_CURRENT_SOURCE_DIR}/../../components/ui/fonts/wavex_mono_*.c)
        target_sources(${test_name} PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/../../components/ui/components/ui_value_tile.cpp
            ${CMAKE_CURRENT_SOURCE_DIR}/../../components/ui/components/ui_dial.cpp
            ${CMAKE_CURRENT_SOURCE_DIR}/../../components/ui/src/multi_touch_input.cpp
            ${PARAMETER_WIDGET_FONTS})
    endif()

    if(test_name STREQUAL "mixer_page_test" OR test_name STREQUAL "project_files_page_test")
        file(GLOB MIXER_FONTS
            ${CMAKE_CURRENT_SOURCE_DIR}/../../components/ui/fonts/wavex_mono_*.c)
        if(test_name STREQUAL "mixer_page_test")
            set(WAVEX_PAGE_SOURCE ui_mixer_page.cpp)
        else()
            set(WAVEX_PAGE_SOURCE ui_project_files_page.cpp)
        endif()
        target_sources(${test_name} PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/../../components/ui/pages/${WAVEX_PAGE_SOURCE}
            ${CMAKE_CURRENT_SOURCE_DIR}/../../components/ui/src/ui_navigator.cpp
            ${CMAKE_CURRENT_SOURCE_DIR}/../../components/ui/src/ui_softkey_bar.cpp
            ${CMAKE_CURRENT_SOURCE_DIR}/../../components/ui/src/current_track.cpp
            ${CMAKE_CURRENT_SOURCE_DIR}/../../components/ui/styles/ui_theme.cpp
            ${MIXER_FONTS})
        target_include_directories(${test_name} PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/stubs
            ${CMAKE_CURRENT_SOURCE_DIR}/../../components/ui/include)
    endif()

    target_link_libraries(${test_name} PRIVATE
        lvgl_host
        Threads::Threads
        GTest::gtest_main
    )

    set_target_properties(${test_name} PROPERTIES
        CXX_STANDARD 17
        CXX_STANDARD_REQUIRED YES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    )

    gtest_discover_tests(${test_name})
endforeach()
