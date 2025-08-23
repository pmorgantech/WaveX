# WaveX SPI Link Implementation

This document describes the new SPI link implementation for WaveX, which provides high-performance communication between the ESP32-S3 frontend and Daisy backend while maintaining backward compatibility with the existing UART link.

## Overview

The SPI link system implements the protocol design from `docs/wxb_full_integration_doc.md` and provides:

- **IRQ-driven communication** using GPIO interrupts for efficiency
- **Lock-free ring buffers** for packet queuing without locks
- **CRC-16/CCITT-FALSE validation** for packet integrity
- **DMA support** for full-duplex SPI communication
- **Fallback safety** with the existing UART link

## Configuration

### Link Selection

The system can switch between SPI and UART using configuration macros:

```cpp
// In firmware/shared/config/link_config.h
#define WAVEX_USE_SPI_LINK 1    // 1 = SPI, 0 = UART
```

### Pin Configuration

#### ESP32-S3 (Master)
```cpp
#define PIN_SPI_SCK       36    // SPI Clock
#define PIN_SPI_MOSI      35    // SPI MOSI (ESP → Daisy)
#define PIN_SPI_MISO      37    // SPI MISO (Daisy → ESP)
#define PIN_SPI_CS        39    // SPI Chip Select
#define PIN_IRQ_DAISY2ESP 34    // Daisy → ESP interrupt
#define PIN_IRQ_ESP2DAISY 33    // ESP → Daisy interrupt (optional)
```

#### Daisy (Slave)
```cpp
// Configure these pins in firmware/daisy/src/comm/inter_spi.h
#define PIN_IRQ_DAISY2ESP  (/* your GPIO here */)
#define PIN_SPI_SCK        (/* your SPI SCK pin */)
#define PIN_SPI_MOSI       (/* your SPI MOSI pin */)
#define PIN_SPI_MISO       (/* your SPI MISO pin */)
#define PIN_SPI_CS         (/* your SPI CS pin */)
```

## Architecture

### Protocol

The SPI link uses a simplified protocol compared to the UART link:

```cpp
typedef struct __attribute__((packed)) {
    uint16_t len;    // bytes in payload (0..240)
    uint16_t type;   // app-defined message type
} pkt_hdr_t;

typedef struct __attribute__((packed)) {
    pkt_hdr_t h;
    uint8_t   payload[240];     // payload data
    uint16_t  crc;              // CRC-16/CCITT-FALSE
} pkt_t;
```

### Message Types

- `0x0000`: Ping/Keepalive
- `0x1000`: Heartbeat
- `0x1001`: Control Change
- `0x1002`: Note On
- `0x1003`: Note Off
- `0x1004`: Sample Control
- `0x1005`: Preview Request

### Ring Buffer System

The system uses lock-free ring buffers for efficient packet queuing:

- **RX Ring**: Receives packets from the other MCU
- **TX Ring**: Queues packets for transmission
- **Free Pools**: Pre-allocated packet pools to avoid malloc in ISR paths

## Usage

### ESP32-S3 Side

```cpp
#include "spi_link.h"

// Initialize SPI link
esp_err_t ret = spi_link_init();
if (ret != ESP_OK) {
    ESP_LOGE(TAG, "SPI link init failed");
    return ret;
}

// Start SPI link
ret = spi_link_start();
if (ret != ESP_OK) {
    ESP_LOGE(TAG, "SPI link start failed");
    return ret;
}

// Send a message
uint8_t payload[4] = {0x01, 0x02, 0x03, 0x04};
if (spi_link_send(0x1001, payload, 4)) {
    ESP_LOGI(TAG, "Message sent successfully");
}

// Receive messages
void* packet;
while (spi_link_recv(&packet)) {
    pkt_t* pkt = (pkt_t*)packet;
    // Process packet based on pkt->h.type
    process_spi_packet(pkt);
    spi_link_recycle(packet, 1);
}
```

### Daisy Side

```cpp
#include "comm/inter_spi.h"

// Initialize SPI link
WaveX::Comm::Spi_Init(hw);

// Send a message
uint8_t payload[4] = {0x01, 0x02, 0x03, 0x04};
if (WaveX::Comm::Spi_Send(0x1001, payload, 4)) {
    hw.PrintLine("Message sent successfully");
}

// Receive messages
pkt_t* packet;
while (WaveX::Comm::Spi_Recv(&packet)) {
    // Process packet based on packet->h.type
    process_spi_packet(packet);
    WaveX::Comm::Spi_Recycle(packet, 1);
}
```

## Building and Testing

### Build Configuration

1. Set `WAVEX_USE_SPI_LINK = 1` in `firmware/shared/config/link_config.h`
2. Configure pin assignments for your hardware
3. Build both ESP32 and Daisy firmware

### Testing

1. **Basic Communication**: Check that heartbeat messages are exchanged
2. **Packet Validation**: Verify CRC validation is working
3. **Performance**: Monitor packet throughput and latency
4. **Error Handling**: Test with invalid packets and connection issues

### Debugging

Enable debug logging by setting:
```cpp
#define WAVEX_UART_DEBUG_LOG 1
```

Monitor the serial output for:
- Link initialization messages
- Packet transmission/reception logs
- Error messages and statistics

## Fallback to UART

If the SPI link fails or you need to debug, you can easily switch back to UART:

1. Set `WAVEX_USE_SPI_LINK = 0` in the config
2. Rebuild and flash
3. The system will automatically use the existing UART implementation

## Performance Characteristics

- **Clock Speed**: 10 MHz SPI (configurable)
- **Packet Size**: Up to 240 bytes payload
- **Latency**: IRQ-driven, typically < 1ms
- **Throughput**: Theoretical max ~10 Mbps (actual depends on packet size and frequency)

## Troubleshooting

### Common Issues

1. **No Communication**: Check pin assignments and SPI configuration
2. **CRC Errors**: Verify signal integrity and timing
3. **High Latency**: Check IRQ configuration and task priorities
4. **Memory Issues**: Verify ring buffer sizes and packet pool allocation

### Debug Commands

- Monitor SPI statistics: `spi_link_get_stats()`
- Check link status: `inter_mcu_is_busy()`
- Toggle debug output: Set `WAVEX_UART_DEBUG_LOG = 1`

## Future Enhancements

- **Dynamic Link Selection**: Runtime switching between SPI and UART
- **Advanced Error Recovery**: Automatic retry and reconnection
- **Performance Monitoring**: Real-time throughput and latency metrics
- **Multi-Channel Support**: Multiple SPI channels for higher bandwidth
