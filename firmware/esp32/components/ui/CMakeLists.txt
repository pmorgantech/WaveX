# UI Components CMakeLists.txt

# Add UI source files
set(UI_SOURCES
    "styles/ui_theme.cpp"
    "common/window_manager.cpp"
    "components/file_browser.cpp"
    "pages/sample_load_save.cpp"
    "pages/diagnostics_page.cpp"
    "src/input_dispatcher.cpp"
    "src/ui_patch_list.cpp"
    "src/ui_param_edit.cpp"
    "src/tca8418_keypad.cpp"
    "src/ui_softkey_bar.cpp"
    "src/ui_navigator.cpp"
    "src/ui_menu_page.cpp"
    "src/ui_navigation_integration.cpp"
    "src/ui_existing_pages.cpp"
)

# Add UI include directories
set(UI_INCLUDE_DIRS
    "styles"
    "common"
    "components"
    "pages"
    "include"
    "."
    "../../main"
    "../../main/comm"
    "../../main/links"
    "../../../shared/config"
    "../../../shared/spi_protocol"
)

# Create UI library
idf_component_register(
    SRCS ${UI_SOURCES}
    INCLUDE_DIRS ${UI_INCLUDE_DIRS}
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
