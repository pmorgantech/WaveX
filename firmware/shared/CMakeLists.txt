# ESP-IDF component registration
idf_component_register(
    SRCS "spi_protocol/protocol.cpp"
         "uart_protocol/uart_protocol.cpp"
         "utils/buffer.cpp"
    INCLUDE_DIRS "."
                 "spi_protocol"
                 "uart_protocol"
                 "utils"
)