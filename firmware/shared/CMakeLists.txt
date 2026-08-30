# ESP-IDF component registration
idf_component_register(
    SRCS "spi_protocol/protocol.cpp"
         "uart_protocol/uart_protocol.cpp"
    INCLUDE_DIRS "."
                 "spi_protocol"
                 "uart_protocol"
)

# First-party warning flags - same delta as firmware/esp32/main/CMakeLists.txt.
# Only affects the ESP32 component build of these sources; the Daisy image and
# the host tests set their own flags in their own build files.
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
