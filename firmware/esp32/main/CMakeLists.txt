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
# silently. Note the same first-party code is built at more than one standard -
# C++23 here, C++17 in all three test trees and in the Daisy device build - so
# anything under firmware/shared/ has to compile as C++17. (This comment said
# C++14 until 2026-09-01; that had been untrue since the Daisy build was raised
# to 17 for logging_config.h's inline variables.)
target_compile_options(${COMPONENT_LIB} PRIVATE $<$<COMPILE_LANGUAGE:CXX>:-std=gnu++2b>)

# First-party warning flags (docs/testing-remediation.md). IDF already builds
# everything with -Wall -Wextra; the delta is -Wconversion plus re-enabling
# the two warnings IDF suppresses globally (later flags win in GCC). Scoped
# per component because IDF's own code and the managed components do not
# build clean under -Wconversion.
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

# ui_task.cpp pulls LVGL headers in via esp_lvgl_port, and LVGL does not build
# clean under -Wconversion (lv_draw_buf.h). Same SYSTEM re-declaration as
# components/ui/CMakeLists.txt - see the comment there for the double name.
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
