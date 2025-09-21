# UI Components CMakeLists.txt

# Add UI source files
set(UI_SOURCES
    "styles/ui_theme.cpp"
    "common/window_manager.cpp"
    "components/file_browser.cpp"
    "pages/sample_load_save.cpp"
    "pages/diagnostics_page.cpp"
)

# Add UI include directories
set(UI_INCLUDE_DIRS
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
    SRCS ${UI_SOURCES}
    INCLUDE_DIRS ${UI_INCLUDE_DIRS}
    REQUIRES 
        lvgl
        esp_lvgl_port
        driver
        esp_driver_gpio
        esp_driver_spi
        esp_common
        shared
)
