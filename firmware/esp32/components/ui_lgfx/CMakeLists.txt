idf_component_register(
    SRCS 
        "src/ui_manager.cpp"
        "src/ui_task.cpp"
        "src/screens_main.cpp"
        "src/screens_display.cpp"
        "src/screensaver.cpp"
        "src/lgfx_device.cpp"
    INCLUDE_DIRS 
        "include"
    REQUIRES 
        hardware_config
        LovyanGFX
    PRIV_REQUIRES
        main
)


