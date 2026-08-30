idf_component_register(
    SRCS
        "main.cpp"
        "application_context.cpp"
        "log_ring.cpp"
        "wavex_application.cpp"
        "inter_mcu.cpp"
        "ui_task.cpp"
        "pcnt_task.cpp"
        "midi_task.cpp"
        "usb_midi_task.cpp"
        "comm/packet_router.cpp"
        "comm/statistics.cpp"
        "comm/comm_interface_impl.cpp"
        "links/esp_spi_link.cpp"
        "links/esp_uart_link.cpp"
    INCLUDE_DIRS
        "."
        "comm"
        "links"
        "../../shared/spi_protocol"
        "../../shared/uart_protocol"
        "../../shared/config"
        "../../shared"
    REQUIRES
        "driver"
        "freertos"
        "esp_timer"
        "esp_common"
        "esp_driver_gpio"
        "esp_driver_spi"
        "esp_driver_i2c"
        "esp_lcd"
        "esp_lvgl_port"
        "esp_lcd_touch"
        "esp_lcd_touch_gt911"
        "esp_mm"
        "esp_app_format"  # esp_app_get_description(): version from the root VERSION file
    PRIV_REQUIRES
        "shared"
        "ui"
)

# Pin the C++ standard rather than inheriting whatever ESP-IDF defaults to.
# IDF 5.5 compiles this as gnu++2b; the guide (docs/esp32p4_coding_guide.md §8)
# asks for the choice to be recorded so a future IDF release cannot move it
# silently. Note the same first-party code is built at three different
# standards - C++23 here, C++17 in firmware/esp32/tests, C++14 in
# firmware/daisy/tests - so anything under firmware/shared/ has to compile as
# C++14.
target_compile_options(${COMPONENT_LIB} PRIVATE $<$<COMPILE_LANGUAGE:CXX>:-std=gnu++2b>)
