#pragma once

#include <stddef.h>
#include <stdint.h>

#include "../../shared/spi_protocol/protocol.h"

// Shared packet handler that both UART and SPI links can use
// This abstracts the packet creation and parsing logic from the link-specific code
class SharedPacketHandler {
   public:
    // Create control change packet
    static size_t create_control_change_packet(
        uint8_t* buffer, size_t buffer_size, uint8_t parameter, uint8_t channel, uint16_t value);

    // Create note on packet
    static size_t create_note_on_packet(
        uint8_t* buffer, size_t buffer_size, uint8_t note, uint8_t velocity, uint8_t channel);

    // Create note off packet
    static size_t create_note_off_packet(uint8_t* buffer,
                                         size_t buffer_size,
                                         uint8_t note,
                                         uint8_t channel);

    // Create sample control packet
    static size_t create_sample_ctrl_packet(
        uint8_t* buffer, size_t buffer_size, uint8_t slot, uint8_t cmd, float rate);

    // Create preview request packet
    static size_t create_preview_req_packet(uint8_t* buffer,
                                            size_t buffer_size,
                                            uint8_t slot,
                                            uint32_t start,
                                            uint32_t end,
                                            uint16_t decim);

    // Calculate checksum for payload
    static uint8_t calculate_checksum(const uint8_t* payload, size_t length);

    // Validate packet header
    static bool validate_packet_header(const uint8_t* data, size_t length);

    // Extract message type from packet
    static uint8_t get_message_type(const uint8_t* data);

    // Extract payload length from packet
    static uint8_t get_payload_length(const uint8_t* data);

    // Extract payload from packet
    static const uint8_t* get_payload(const uint8_t* data);

    // Get total packet size (header + payload + checksum)
    static size_t get_total_packet_size(const uint8_t* data);
};
