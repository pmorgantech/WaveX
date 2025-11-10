#include "uart_protocol.h"

#include <gtest/gtest.h>

#include "../utils/test_helpers.h"

using namespace WaveX::UartProtocol;
using namespace WaveX::Test;

class UartProtocolTest : public ::testing::Test {
   protected:
    void SetUp() override { buffer_.resize(4096); }

    std::vector<uint8_t> buffer_;
};

// Test packet creation with empty payload
TEST_F(UartProtocolTest, CreatePacketEmptyPayload) {
    uint8_t msg_type = 0x12;  // MSG_HEARTBEAT
    uint16_t sequence = 0x1234;
    uint8_t flags = 0;

    size_t result =
        CreateUartPacket(buffer_.data(), buffer_.size(), msg_type, nullptr, 0, sequence, flags);

    EXPECT_GT(result, 0);
    EXPECT_EQ(result, UART_FRAME_OVERHEAD);
    EXPECT_EQ(buffer_[0], UART_START_BYTE);
    EXPECT_EQ(buffer_[result - 1], UART_END_BYTE);
}

// Test packet creation with payload
TEST_F(UartProtocolTest, CreatePacketWithPayload) {
    uint8_t msg_type = 0x01;  // MSG_CONTROL_CHANGE
    uint16_t sequence = 0x5678;
    uint8_t flags = 0;

    struct {
        uint8_t parameter;
        uint8_t channel;
        uint16_t value;
    } payload = {0x01, 0x02, 0x1234};

    size_t result = CreateUartPacket(
        buffer_.data(), buffer_.size(), msg_type, &payload, sizeof(payload), sequence, flags);

    EXPECT_GT(result, 0);
    EXPECT_EQ(result, UART_FRAME_OVERHEAD + sizeof(payload));
    EXPECT_EQ(buffer_[0], UART_START_BYTE);
    EXPECT_EQ(buffer_[result - 1], UART_END_BYTE);
}

// Test packet parsing
TEST_F(UartProtocolTest, ParsePacket) {
    uint8_t msg_type = 0x12;
    uint16_t sequence = 0xABCD;
    uint8_t flags = UART_FLAG_ACK;

    struct {
        uint32_t uptime_ms;
        uint32_t rx_total;
    } payload = {1000, 5000};

    size_t created = CreateUartPacket(
        buffer_.data(), buffer_.size(), msg_type, &payload, sizeof(payload), sequence, flags);

    ASSERT_GT(created, 0);

    uint8_t parsed_type;
    uint8_t parsed_flags;
    uint16_t parsed_seq;
    std::vector<uint8_t> parsed_payload(sizeof(payload));
    size_t parsed_payload_size = parsed_payload.size();

    bool result = ParseUartPacket(buffer_.data(),
                                  created,
                                  parsed_type,
                                  parsed_payload.data(),
                                  parsed_payload_size,
                                  parsed_seq,
                                  parsed_flags);

    EXPECT_TRUE(result);
    EXPECT_EQ(parsed_type, msg_type);
    EXPECT_EQ(parsed_seq, sequence);
    EXPECT_EQ(parsed_flags, flags);
    EXPECT_EQ(parsed_payload_size, sizeof(payload));
    EXPECT_EQ(memcmp(parsed_payload.data(), &payload, sizeof(payload)), 0);
}

// Test frame validation
TEST_F(UartProtocolTest, ValidateFrame) {
    uint8_t msg_type = 0x12;
    uint16_t sequence = 0x0001;

    size_t created =
        CreateUartPacket(buffer_.data(), buffer_.size(), msg_type, nullptr, 0, sequence, 0);

    ASSERT_GT(created, 0);

    EXPECT_TRUE(ValidateUartFrame(buffer_.data(), created));
}

// Test invalid frame detection (wrong start byte)
TEST_F(UartProtocolTest, ValidateFrameInvalidStart) {
    uint8_t msg_type = 0x12;
    uint16_t sequence = 0x0001;

    size_t created =
        CreateUartPacket(buffer_.data(), buffer_.size(), msg_type, nullptr, 0, sequence, 0);

    ASSERT_GT(created, 0);

    buffer_[0] = 0xFF;  // Corrupt start byte
    EXPECT_FALSE(ValidateUartFrame(buffer_.data(), created));
}

// Test invalid frame detection (wrong end byte)
TEST_F(UartProtocolTest, ValidateFrameInvalidEnd) {
    uint8_t msg_type = 0x12;
    uint16_t sequence = 0x0001;

    size_t created =
        CreateUartPacket(buffer_.data(), buffer_.size(), msg_type, nullptr, 0, sequence, 0);

    ASSERT_GT(created, 0);

    buffer_[created - 1] = 0xFF;  // Corrupt end byte
    EXPECT_FALSE(ValidateUartFrame(buffer_.data(), created));
}

// Test invalid frame detection (wrong CRC)
TEST_F(UartProtocolTest, ValidateFrameInvalidCRC) {
    uint8_t msg_type = 0x12;
    uint16_t sequence = 0x0001;

    size_t created =
        CreateUartPacket(buffer_.data(), buffer_.size(), msg_type, nullptr, 0, sequence, 0);

    ASSERT_GT(created, 0);

    buffer_[created - 3] ^= 0xFF;  // Corrupt CRC
    EXPECT_FALSE(ValidateUartFrame(buffer_.data(), created));
}

// Test frame too small
TEST_F(UartProtocolTest, ValidateFrameTooSmall) {
    EXPECT_FALSE(ValidateUartFrame(buffer_.data(), UART_FRAME_OVERHEAD - 1));
}

// Test FindFrameStart
TEST_F(UartProtocolTest, FindFrameStart) {
    // Create a packet
    size_t created = CreateUartPacket(buffer_.data(), buffer_.size(), 0x12, nullptr, 0, 0x0001, 0);

    ASSERT_GT(created, 0);

    // Add some garbage before the frame
    std::vector<uint8_t> test_buffer(created + 10);
    test_buffer[0] = 0x00;
    test_buffer[1] = 0xFF;
    memcpy(&test_buffer[2], buffer_.data(), created);

    int start_pos = FindFrameStart(test_buffer.data(), test_buffer.size());
    EXPECT_EQ(start_pos, 2);
}

// Test FindFrameStart with no valid frame
TEST_F(UartProtocolTest, FindFrameStartNotFound) {
    std::vector<uint8_t> test_buffer(100, 0xFF);
    int start_pos = FindFrameStart(test_buffer.data(), test_buffer.size());
    EXPECT_LT(start_pos, 0);
}

// Test GetFrameLength
TEST_F(UartProtocolTest, GetFrameLength) {
    uint8_t msg_type = 0x12;
    size_t payload_size = 10;
    std::vector<uint8_t> payload(payload_size, 0xAA);

    size_t created = CreateUartPacket(
        buffer_.data(), buffer_.size(), msg_type, payload.data(), payload_size, 0x0001, 0);

    ASSERT_GT(created, 0);

    size_t frame_length = GetFrameLength(buffer_.data(), buffer_.size());
    EXPECT_EQ(frame_length, created);
}

// Test sequence number wraparound
TEST_F(UartProtocolTest, SequenceWraparound) {
    uint16_t sequences[] = {0xFFFF, 0x0000, 0x0001};

    for (auto seq: sequences) {
        size_t created = CreateUartPacket(buffer_.data(), buffer_.size(), 0x12, nullptr, 0, seq, 0);

        ASSERT_GT(created, 0);

        uint8_t parsed_type;
        uint8_t parsed_flags;
        uint16_t parsed_seq;
        std::vector<uint8_t> parsed_payload(1);
        size_t parsed_payload_size = 0;

        bool result = ParseUartPacket(buffer_.data(),
                                      created,
                                      parsed_type,
                                      parsed_payload.data(),
                                      parsed_payload_size,
                                      parsed_seq,
                                      parsed_flags);

        EXPECT_TRUE(result);
        EXPECT_EQ(parsed_seq, seq);
    }
}

// Test flags encoding/decoding
TEST_F(UartProtocolTest, FlagsEncoding) {
    uint8_t flags[] = {0, UART_FLAG_ACK, UART_FLAG_NACK, UART_FLAG_ACK | UART_FLAG_NACK};

    for (auto flag: flags) {
        size_t created =
            CreateUartPacket(buffer_.data(), buffer_.size(), 0x12, nullptr, 0, 0x0001, flag);

        ASSERT_GT(created, 0);

        uint8_t parsed_type;
        uint8_t parsed_flags;
        uint16_t parsed_seq;
        std::vector<uint8_t> parsed_payload(1);
        size_t parsed_payload_size = 0;

        bool result = ParseUartPacket(buffer_.data(),
                                      created,
                                      parsed_type,
                                      parsed_payload.data(),
                                      parsed_payload_size,
                                      parsed_seq,
                                      parsed_flags);

        EXPECT_TRUE(result);
        EXPECT_EQ(parsed_flags, flag);
    }
}

// Test maximum payload size
TEST_F(UartProtocolTest, MaximumPayloadSize) {
    std::vector<uint8_t> large_payload(UART_MAX_PAYLOAD, 0xAA);

    size_t result = CreateUartPacket(buffer_.data(),
                                     buffer_.size(),
                                     0x12,
                                     large_payload.data(),
                                     large_payload.size(),
                                     0x0001,
                                     0);

    EXPECT_GT(result, 0);
}

// Test payload size exceeds maximum
TEST_F(UartProtocolTest, PayloadExceedsMaximum) {
    std::vector<uint8_t> too_large_payload(UART_MAX_PAYLOAD + 1, 0xAA);

    size_t result = CreateUartPacket(buffer_.data(),
                                     buffer_.size(),
                                     0x12,
                                     too_large_payload.data(),
                                     too_large_payload.size(),
                                     0x0001,
                                     0);

    EXPECT_EQ(result, 0);
}

// Test buffer too small
TEST_F(UartProtocolTest, BufferTooSmall) {
    uint8_t msg_type = 0x12;
    std::vector<uint8_t> payload(100, 0xAA);
    std::vector<uint8_t> small_buffer(10);  // Too small

    size_t result = CreateUartPacket(small_buffer.data(),
                                     small_buffer.size(),
                                     msg_type,
                                     payload.data(),
                                     payload.size(),
                                     0x0001,
                                     0);

    EXPECT_EQ(result, 0);
}

// Test CRC calculation consistency
TEST_F(UartProtocolTest, CRCConsistency) {
    const uint8_t test_data[] = "Hello, World!";
    size_t data_len = sizeof(test_data) - 1;  // Exclude null terminator

    uint16_t crc1 = CalculateUartCrc(test_data, data_len);
    uint16_t crc2 = CalculateUartCrc(test_data, data_len);

    EXPECT_EQ(crc1, crc2);
}

// Test CRC with different data produces different CRC
TEST_F(UartProtocolTest, CRCDifferentData) {
    const uint8_t data1[] = "Hello";
    const uint8_t data2[] = "World";

    uint16_t crc1 = CalculateUartCrc(data1, sizeof(data1) - 1);
    uint16_t crc2 = CalculateUartCrc(data2, sizeof(data2) - 1);

    EXPECT_NE(crc1, crc2);
}

// Test CRC with empty data
TEST_F(UartProtocolTest, CRCEmptyData) {
    uint16_t crc = CalculateUartCrc(nullptr, 0);
    EXPECT_EQ(crc, 0);
}

// Test round-trip: create -> parse -> verify
TEST_F(UartProtocolTest, RoundTrip) {
    struct {
        uint32_t value1;
        uint16_t value2;
        uint8_t value3;
    } original_payload = {0x12345678, 0xABCD, 0x42};

    size_t created = CreateUartPacket(buffer_.data(),
                                      buffer_.size(),
                                      0x12,
                                      &original_payload,
                                      sizeof(original_payload),
                                      0x1234,
                                      UART_FLAG_ACK);

    ASSERT_GT(created, 0);

    uint8_t parsed_type;
    uint8_t parsed_flags;
    uint16_t parsed_seq;
    decltype(original_payload) parsed_payload;
    size_t parsed_payload_size = sizeof(parsed_payload);

    bool result = ParseUartPacket(buffer_.data(),
                                  created,
                                  parsed_type,
                                  reinterpret_cast<uint8_t*>(&parsed_payload),
                                  parsed_payload_size,
                                  parsed_seq,
                                  parsed_flags);

    EXPECT_TRUE(result);
    EXPECT_EQ(parsed_type, 0x12);
    EXPECT_EQ(parsed_seq, 0x1234);
    EXPECT_EQ(parsed_flags, UART_FLAG_ACK);
    EXPECT_EQ(parsed_payload_size, sizeof(original_payload));
    EXPECT_EQ(memcmp(&parsed_payload, &original_payload, sizeof(original_payload)), 0);
}
