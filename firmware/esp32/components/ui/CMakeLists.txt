# UI Components CMakeLists.txt

# Add UI source files
set(COMPONENT_SRCS
    "common/window_manager.cpp"
    "components/file_browser.cpp"
    "pages/diagnostics_page.cpp"
    "pages/sample_load_save.cpp"
    "src/input_dispatcher.cpp"
    "src/tca8418_keypad.cpp"
    "src/ui_existing_pages.cpp"
    "src/ui_file_browser.cpp"
    "src/ui_main_menu.cpp"
    "src/ui_main.cpp"
    "src/ui_menu_page.cpp"
    "src/ui_navigation_demo.cpp"
    "src/ui_navigation_integration.cpp"
    "src/ui_navigator.cpp"
    "src/ui_param_edit.cpp"
    "src/ui_patch_list.cpp"
    "src/ui_sample_detail.cpp"
    "src/ui_settings_page.cpp"
    "src/ui_softkey_bar.cpp"
    "styles/ui_theme.cpp"
    "ui_globals.cpp"
)

# Add UI include directories
set(COMPONENT_INCLUDE_DIRS
    "styles"
    "common"
    "components"
    "pages"
    "."
    "../../main"
    "../../main/comm"
    "../../main/links"
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
        driver
        esp_tca8418
        esp_driver_gpio
        esp_driver_spi
        esp32_p4_nano
        esp_common
        shared
)
