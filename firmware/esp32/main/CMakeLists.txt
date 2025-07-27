idf_component_register(SRCS "main.cpp" "ui_main.cpp" "ui_about.cpp" "inter_mcu.cpp"
                       INCLUDE_DIRS "."
                       REQUIRES esp_lvgl_port hardware_config ft6x36_idf shared
                       PRIV_REQUIRES app_update) 