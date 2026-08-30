# UI Components CMakeLists.txt

# Add UI source files
set(COMPONENT_SRCS
    "components/envelope_cache.cpp"
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
# silently. Note the same first-party code is built at three different
# standards - C++23 here, C++17 in firmware/esp32/tests, C++14 in
# firmware/daisy/tests - so anything under firmware/shared/ has to compile as
# C++14.
target_compile_options(${COMPONENT_LIB} PRIVATE $<$<COMPILE_LANGUAGE:CXX>:-std=gnu++2b>)
