# UI Components CMakeLists.txt

# Add UI source files
set(COMPONENT_SRCS
    "components/envelope_cache.cpp"
    "components/envelope_cache_init.cpp"
    "components/envelope_fetcher.cpp"
    "components/envelope_panel.cpp"
    "components/envelope_panel_link.cpp"
    "components/file_browser.cpp"
    "components/waveform_view.cpp"
    "components/ui_value_tile.cpp"
    "components/encoder_strip.cpp"
    "pages/ui_pot_cal_page.cpp"
    "components/ui_dial.cpp"
    "src/ui_diagnostics_page.cpp"
    "src/ui_screenshot.cpp"
    "src/ui_console.cpp"
    "src/input_dispatcher.cpp"
    "src/tca8418_keypad.cpp"
    "src/ui_api.cpp"
    "src/ui_busy_overlay.cpp"
    "src/ui_main_menu.cpp"
    "src/panel_key.cpp"
    "src/ui_menu_page.cpp"
    "src/ui_navigation_integration.cpp"
    "src/ui_navigator.cpp"
    "src/display_manager.cpp"
    "src/multi_touch_input.cpp"
    "src/multi_touch_port.cpp"
    "src/ui_settings_page.cpp"
    "src/ui_softkey_bar.cpp"
    "src/ui_tab_group.cpp"
    "src/ui_tab_host_page.cpp"
    "src/ui_status_strip.cpp"
    "src/ui_sample_browser.cpp"
    "src/current_sample.cpp"
    "src/current_track.cpp"
    "pages/ui_play_page.cpp"
    "pages/ui_sequencer_page.cpp"
    "pages/ui_pattern_files_page.cpp"
    "pages/ui_project_files_page.cpp"
    "pages/ui_bank_page.cpp"
    "pages/ui_pattern_slots_page.cpp"
    "pages/ui_song_page.cpp"
    "pages/ui_pad_map_page.cpp"
    "pages/ui_pad_sound_page.cpp"
    "pages/ui_allocation_page.cpp"
    "pages/ui_notes_page.cpp"
    "pages/ui_track_page.cpp"
    "pages/ui_mixer_page.cpp"
    "pages/ui_sample_manager_page.cpp"
    "pages/ui_system_info_page.cpp"
    "pages/ui_instrument_page.cpp"
    "pages/ui_sample_record_page.cpp"
    "pages/ui_cv_cal_page.cpp"
    "pages/ui_storage_page.cpp"
    "pages/ui_sample_edit_page.cpp"
    "pages/ui_sample_file_page.cpp"
    "styles/ui_theme.cpp"
    "fonts/wavex_mono_14.c"
    "fonts/wavex_mono_18.c"
    "fonts/wavex_mono_26.c"
    "fonts/wavex_mono_38.c"
    "fonts/wavex_mono_48.c"
)

# Add UI include directories
set(COMPONENT_INCLUDE_DIRS
    "styles"
    "fonts"
    "components"
    "pages"
    "."
    "../../../shared/config"
    "../../../shared/spi_protocol"
    "../../../shared"
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
        esp_driver_usb_serial_jtag
        esp_driver_i2c
        esp_driver_gpio
        esp_driver_spi
        esp32_p4_nano
        esp_common
        esp_app_format
        esp_timer
        shared
        main
)

# Compile-time theme selection. The palette lives in styles/themes/; this
# picks which one styles/ui_theme.h includes. Themes only re-map colours -
# layout, type and geometry are identical across all four - so switching one
# cannot change whether a page fits.
set(WAVEX_UI_THEME "neutral" CACHE STRING "WaveX UI palette")
set_property(CACHE WAVEX_UI_THEME PROPERTY STRINGS neutral amber teal contrast)
string(TOUPPER "${WAVEX_UI_THEME}" WAVEX_UI_THEME_UPPER)
if(NOT WAVEX_UI_THEME MATCHES "^(neutral|amber|teal|contrast)$")
    message(FATAL_ERROR
            "WAVEX_UI_THEME must be neutral, amber, teal or contrast "
            "(got '${WAVEX_UI_THEME}')")
endif()
# Public, not private: ui_theme.h is included by main/ and by other components,
# and a translation unit that picked a different palette would lay out against
# a different set of colours than the one the UI component drew with.
target_compile_definitions(${COMPONENT_LIB}
                           PUBLIC WAVEX_UI_THEME_${WAVEX_UI_THEME_UPPER})

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

# The BSP creates the controller once; WaveX adapts its public registration
# entry points to independent LVGL pointers. Never patch managed components.
target_link_options(${COMPONENT_LIB} INTERFACE
    "LINKER:--wrap=lvgl_port_add_touch"
    "LINKER:--wrap=lvgl_port_remove_touch")
