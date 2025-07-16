idf_component_register(SRCS "main.cpp"
                              "ui_main.cpp"
                              "ui_about.cpp"
                    INCLUDE_DIRS "."
                    REQUIRES esp_lvgl_port hardware_config driver esp_lcd) 