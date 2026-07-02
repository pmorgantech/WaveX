/**
 * @file application_context.cpp
 * @brief Application Context Implementation
 */

#include "application_context.h"

#include "comm/comm_interface_impl.h"
#include "links/esp_uart_link.h"
#if WAVEX_SPI_LINK_ENABLED
#include "links/esp_spi_link.h"
#endif

namespace WaveX {

ApplicationContext::ApplicationContext()
    : statistics_(std::make_unique<StatisticsManager>()),
      packet_router_(std::make_unique<WaveX::Comm::PacketRouter>()),
      comm_interface_(std::make_unique<WaveX::Comm::CommInterfaceImpl>(*statistics_)) {
    // Initialize link components with injected dependencies
    initializeLinks();
}

void ApplicationContext::initializeLinks() {
    // Inject PacketRouter into SPI link if enabled
#if WAVEX_SPI_LINK_ENABLED
    ::spi_link_set_packet_router(packet_router_.get());
#endif

    // Inject PacketRouter into UART link
    ::uart_link_set_packet_router(packet_router_.get());
}

}  // namespace WaveX
