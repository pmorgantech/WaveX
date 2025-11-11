#ifndef WAVEX_ESP_UART_LINK_H
#define WAVEX_ESP_UART_LINK_H

#include <stddef.h>
#include <stdint.h>

#include "link_config.h"

// Forward declaration for PacketRouter
namespace WaveX {
namespace Comm {
class PacketRouter;
}
}  // namespace WaveX

#ifdef ESP_PLATFORM
#include "esp_err.h"
#else
typedef int esp_err_t;
#ifndef ESP_OK
#define ESP_OK 0
#endif
#endif

#ifdef __cplusplus
extern "C" {
#endif

// Initialize UART hardware and DMA (must be called before uart_link_start)
esp_err_t uart_link_init(void);

// Start UART tasks and begin communication
esp_err_t uart_link_start(void);

// Queue message for transmission over UART (returns payload length or <0 on error)
int uart_link_send(uint16_t msg_type, const void* payload, uint16_t len);

// Stop UART subsystem and release resources
esp_err_t uart_link_stop(void);

// Log UART statistics for debugging
void uart_link_log_stats(void);

#ifdef __cplusplus
}

// Set PacketRouter reference for dependency injection (C++ function)
void uart_link_set_packet_router(WaveX::Comm::PacketRouter* packet_router);
#endif

#endif  // WAVEX_ESP_UART_LINK_H
