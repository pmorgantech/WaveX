idf_component_register(SRCS "main.cpp" "inter_mcu.cpp"
                       INCLUDE_DIRS "."
                       REQUIRES hardware_config shared ui_lgfx
                       PRIV_REQUIRES app_update)