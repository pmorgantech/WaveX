# UI Components CMakeLists.txt

# Add UI source files
set(COMPONENT_SRCS
    "components/envelope_cache.cpp"
    "components/envelope_cache_init.cpp"
    "components/envelope_fetcher.cpp"
    "components/file_browser.cpp"
    "components/waveform_view.cpp"
    "src/ui_diagnostics_page.cpp"
    "src/ui_screenshot.cpp"
    "src/input_dispatcher.cpp"
    "src/tca8418_keypad.cpp"
    "src/ui_api.cpp"
    "src/ui_busy_overlay.cpp"
    "src/ui_main_menu.cpp"
    "src/ui_menu_page.cpp"
    "src/ui_navigation_integration.cpp"
    "src/ui_navigator.cpp"
    "src/display_manager.cpp"
    "src/ui_settings_page.cpp"
    "src/ui_softkey_bar.cpp"
    "src/ui_tab_group.cpp"
    "src/ui_tab_host_page.cpp"
    "src/ui_status_strip.cpp"
    "src/ui_sample_browser.cpp"
    "src/current_sample.cpp"
    "pages/ui_play_page.cpp"
    "pages/ui_sample_manager_page.cpp"
    "pages/ui_system_info_page.cpp"
    "pages/ui_voice_page.cpp"
    "pages/ui_sample_record_page.cpp"
    "pages/ui_cv_cal_page.cpp"
    "pages/ui_sample_edit_page.cpp"
    "styles/ui_theme.cpp"
)

# Add UI include directories
set(COMPONENT_INCLUDE_DIRS
    "styles"
    "components"
    "pages"
    "."
    "../../../shared/config"
    "../../../shared/spi_protocol"
)

# Create UI library
idf_component_register(
    SRCS ${COMPONENT_SRCS}
    INCLUDE_DIRS "include" ${COMPONENT_INCLUDE_DIRS}
    REQUIRES
        lvgl
        esp_lvgl_port
        esp_lcd_touch_gt911
        driver
        esp_tca8418
        esp_driver_gpio
        esp_driver_spi
        esp32_p4_nano
        esp_common
        esp_app_format
        esp_timer
        shared
        main
)

# Pin the C++ standard rather than inheriting whatever ESP-IDF defaults to.
# IDF 5.5 compiles this as gnu++2b; the guide (docs/esp32p4_coding_guide.md §8)
# asks for the choice to be recorded so a future IDF release cannot move it
# silently. Note the same first-party code is built at more than one standard -
# C++23 here, C++17 in all three test trees and in the Daisy device build - so
# anything under firmware/shared/ has to compile as C++17. (This comment said
# C++14 until 2026-09-01; that had been untrue since the Daisy build was raised
# to 17 for logging_config.h's inline variables.)
target_compile_options(${COMPONENT_LIB} PRIVATE $<$<COMPILE_LANGUAGE:CXX>:-std=gnu++2b>)

# First-party warning flags - same delta as main/CMakeLists.txt: IDF already
# applies -Wall -Wextra, this adds -Wconversion and re-enables what IDF
# suppresses globally. Scoped per component; LVGL and the managed components
# do not build clean under -Wconversion.
target_compile_options(
    ${COMPONENT_LIB}
    PRIVATE -Wconversion
            -Wunused-parameter
            -Wsign-compare
            # Warnings, not errors (same policy as the host suites) - IDF's
            # -Werror=all would otherwise escalate the re-enabled -Wall-family
            # warnings, and a hard failure on a toolchain bump is worse than a
            # visible warning.
            -Wno-error=conversion
            -Wno-error=unused-parameter
            -Wno-error=sign-compare)

# LVGL's own headers do not build clean under -Wconversion (inline helpers in
# lv_draw_buf.h fire in every TU that includes lvgl.h). Re-declare its
# interface include dirs as SYSTEM so GCC consumes them via -isystem and
# ignores IDF's duplicate -I. Extend the loop if another vendor component's
# headers start warning; first-party components stay out of it on purpose.
# Both names guarded: the managed component registers as lvgl__lvgl on a
# fresh dependency resolution, but a build tree configured before the manager
# ran may know it as plain lvgl - resolving a name that does not exist is a
# hard CMake error, so membership is checked first.
idf_build_get_property(wavex_build_components BUILD_COMPONENTS)
foreach(dep lvgl__lvgl lvgl)
    if(NOT dep IN_LIST wavex_build_components)
        continue()
    endif()
    idf_component_get_property(dep_lib ${dep} COMPONENT_LIB)
    get_target_property(dep_incs ${dep_lib} INTERFACE_INCLUDE_DIRECTORIES)
    if(dep_incs)
        target_include_directories(${COMPONENT_LIB} SYSTEM PRIVATE ${dep_incs})
    endif()
endforeach()
