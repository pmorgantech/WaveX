#include <gtest/gtest.h>

#include "../../shared/spi_protocol/protocol.h"
#include "../mocks/esp32_mocks.h"
#include "packet_router.h"
#include "uart_protocol.h"

#include <cstring>
#include <functional>
#include <vector>

using namespace WaveX::Comm;
using namespace WaveX::Protocol;
using namespace WaveX::UartProtocol;

// Test fixture for inter-MCU protocol integration tests
class InterMcuProtocolIntegrationTest : public ::testing::Test {
   protected:
    void SetUp() override {
        // Create packet router instance
        router_ = std::make_unique<WaveX::Comm::PacketRouter>();

        // Clear handler invocation tracking
        heartbeat_received_ = false;
        meter_received_ = false;
        browse_resp_received_ = false;
        wave_chunk_received_ = false;
        sync_received_ = false;

        heartbeat_data_ = {};
        meter_data_ = {};
        browse_resp_data_.clear();
        wave_chunk_data_ = {};

        // Set up handler callbacks to track invocations
        setup_handlers();
    }

    void TearDown() override {
        // Cleanup
    }

    // Helper: Create a UART packet from a message
    std::vector<uint8_t> CreateUartPacketFromMessage(uint8_t msg_type,
                                                     const void* payload,
                                                     size_t payload_size,
                                                     uint16_t seq = 0,
                                                     uint8_t flags = 0) {
        std::vector<uint8_t> buffer(4096);
        size_t frame_size = CreateUartPacket(
            buffer.data(), buffer.size(), msg_type, payload, payload_size, seq, flags);

        if (frame_size > 0) {
            buffer.resize(frame_size);
        } else {
            buffer.clear();
        }

        return buffer;
    }

    // Helper: Simulate receiving a UART packet and routing it
    bool SimulateReceiveAndRoute(const std::vector<uint8_t>& uart_frame) {
        if (uart_frame.empty()) {
            return false;
        }

        // Parse UART packet
        uint8_t msg_type;
        uint8_t flags;
        uint16_t seq;
        uint8_t payload[2048];
        size_t payload_size = sizeof(payload);

        if (!ParseUartPacket(uart_frame.data(),
                             uart_frame.size(),
                             msg_type,
                             payload,
                             payload_size,
                             seq,
                             flags)) {
            return false;
        }

        // Route the message
        router_->route_packet(payload, payload_size);

        return true;
    }

    // Helper: Full pipeline test - create message, serialize, parse, route
    bool TestFullPipeline(uint8_t msg_type, const void* payload, size_t payload_size) {
        // Step 1: Create UART packet
        std::vector<uint8_t> uart_frame =
            CreateUartPacketFromMessage(msg_type, payload, payload_size);
        if (uart_frame.empty()) {
            return false;
        }

        // Step 2: Validate frame
        if (!ValidateUartFrame(uart_frame.data(), uart_frame.size())) {
            return false;
        }

        // Step 3: Parse and route
        return SimulateReceiveAndRoute(uart_frame);
    }

    // Setup handler tracking
    void setup_handlers() {
        // Note: In real implementation, handlers are set via inter_mcu callbacks
        // For testing, we track invocations through the packet router
    }

    // Handler invocation tracking
    bool heartbeat_received_;
    bool meter_received_;
    bool browse_resp_received_;
    bool wave_chunk_received_;
    bool sync_received_;

    HeartbeatMessage heartbeat_data_;
    MeterPushMessage meter_data_;
    std::vector<uint8_t> browse_resp_data_;
    WaveChunkMessage wave_chunk_data_;
    SyncMessage sync_data_;

    std::unique_ptr<PacketRouter> router_;
};

// Test: End-to-end heartbeat message flow
TEST_F(InterMcuProtocolIntegrationTest, HeartbeatMessageFlow) {
    HeartbeatMessage heartbeat(12345, 100, 200);

    // Test full pipeline
    bool success = TestFullPipeline(MSG_HEARTBEAT, &heartbeat, sizeof(heartbeat));
    EXPECT_TRUE(success);

    // Verify packet was valid
    std::vector<uint8_t> frame =
        CreateUartPacketFromMessage(MSG_HEARTBEAT, &heartbeat, sizeof(heartbeat));
    EXPECT_TRUE(ValidateUartFrame(frame.data(), frame.size()));
}

// Test: End-to-end meter push message flow
TEST_F(InterMcuProtocolIntegrationTest, MeterPushMessageFlow) {
    MeterPushMessage meter(0x4000,  // rms_left, Q15: 0.5
                           0x6000,  // rms_right, Q15: 0.75
                           0x7FFF,  // peak_left, Q15: 1.0
                           0x5000   // peak_right, Q15: 0.625
    );

    bool success = TestFullPipeline(MSG_METER_PUSH, &meter, sizeof(meter));
    EXPECT_TRUE(success);

    // Verify data integrity through pipeline
    std::vector<uint8_t> frame = CreateUartPacketFromMessage(MSG_METER_PUSH, &meter, sizeof(meter));

    uint8_t parsed_type;
    uint8_t parsed_flags;
    uint16_t parsed_seq;
    uint8_t parsed_payload[sizeof(meter)];
    size_t parsed_size = sizeof(parsed_payload);

    bool parsed = ParseUartPacket(frame.data(),
                                  frame.size(),
                                  parsed_type,
                                  parsed_payload,
                                  parsed_size,
                                  parsed_seq,
                                  parsed_flags);
    ASSERT_TRUE(parsed);
    EXPECT_EQ(parsed_type, MSG_METER_PUSH);
    EXPECT_EQ(parsed_size, sizeof(meter));

    MeterPushMessage* parsed_meter = reinterpret_cast<MeterPushMessage*>(parsed_payload);
    EXPECT_EQ(parsed_meter->rms_left, meter.rms_left);
    EXPECT_EQ(parsed_meter->rms_right, meter.rms_right);
    EXPECT_EQ(parsed_meter->peak_left, meter.peak_left);
    EXPECT_EQ(parsed_meter->peak_right, meter.peak_right);
}

// Test: End-to-end browse response message flow (large payload)
TEST_F(InterMcuProtocolIntegrationTest, BrowseResponseMessageFlow) {
    // Create a browse response with multiple file entries
    struct BrowseRespPayload {
        uint32_t total_files;
        uint8_t current_page_entries;
        uint8_t entries[20 * 64];  // 20 entries, 64 bytes each
    } browse_resp;

    browse_resp.total_files = 50;
    browse_resp.current_page_entries = 20;

    // Fill with test data
    for (int i = 0; i < 20; i++) {
        char* entry = reinterpret_cast<char*>(&browse_resp.entries[i * 64]);
        snprintf(entry, 64, "file%02d.wav", i);
    }

    size_t payload_size =
        sizeof(browse_resp.total_files) + sizeof(browse_resp.current_page_entries) + (20 * 64);

    bool success = TestFullPipeline(MSG_BROWSE_RESP, &browse_resp, payload_size);
    EXPECT_TRUE(success);

    // Verify large payload integrity
    std::vector<uint8_t> frame =
        CreateUartPacketFromMessage(MSG_BROWSE_RESP, &browse_resp, payload_size);
    EXPECT_TRUE(ValidateUartFrame(frame.data(), frame.size()));

    uint8_t parsed_type;
    uint8_t parsed_flags;
    uint16_t parsed_seq;
    uint8_t parsed_payload[2048];
    size_t parsed_size = sizeof(parsed_payload);

    bool parsed = ParseUartPacket(frame.data(),
                                  frame.size(),
                                  parsed_type,
                                  parsed_payload,
                                  parsed_size,
                                  parsed_seq,
                                  parsed_flags);
    ASSERT_TRUE(parsed);
    EXPECT_EQ(parsed_type, MSG_BROWSE_RESP);
    EXPECT_EQ(parsed_size, payload_size);

    // Verify first entry
    const char* first_entry =
        reinterpret_cast<const char*>(&parsed_payload[sizeof(uint32_t) + sizeof(uint8_t)]);
    EXPECT_STREQ(first_entry, "file00.wav");
}

// Test: Control change message flow (ESP32 -> Daisy direction)
TEST_F(InterMcuProtocolIntegrationTest, ControlChangeMessageFlow) {
    ControlChangeMessage control(0x42, 0x01, 0x1234);

    bool success = TestFullPipeline(MSG_CONTROL_CHANGE, &control, sizeof(control));
    EXPECT_TRUE(success);

    // Verify round-trip data integrity
    std::vector<uint8_t> frame =
        CreateUartPacketFromMessage(MSG_CONTROL_CHANGE, &control, sizeof(control));

    uint8_t parsed_type;
    uint8_t parsed_flags;
    uint16_t parsed_seq;
    uint8_t parsed_payload[sizeof(control)];
    size_t parsed_size = sizeof(parsed_payload);

    bool parsed = ParseUartPacket(frame.data(),
                                  frame.size(),
                                  parsed_type,
                                  parsed_payload,
                                  parsed_size,
                                  parsed_seq,
                                  parsed_flags);
    ASSERT_TRUE(parsed);

    ControlChangeMessage* parsed_control = reinterpret_cast<ControlChangeMessage*>(parsed_payload);
    EXPECT_EQ(parsed_control->parameter, control.parameter);
    EXPECT_EQ(parsed_control->channel, control.channel);
    EXPECT_EQ(parsed_control->value, control.value);
}

// Test: Error recovery - corrupted CRC
TEST_F(InterMcuProtocolIntegrationTest, ErrorRecoveryCorruptedCRC) {
    HeartbeatMessage heartbeat(1000, 50, 100, 250, 200, 300);

    // Create valid packet
    std::vector<uint8_t> frame =
        CreateUartPacketFromMessage(MSG_HEARTBEAT, &heartbeat, sizeof(heartbeat));
    ASSERT_FALSE(frame.empty());

    // Corrupt CRC (last 2 bytes before END byte)
    size_t crc_offset = frame.size() - 3;  // CRC is 2 bytes before END
    frame[crc_offset] ^= 0xFF;
    frame[crc_offset + 1] ^= 0xFF;

    // Should fail validation
    EXPECT_FALSE(ValidateUartFrame(frame.data(), frame.size()));

    // Should fail parsing
    uint8_t msg_type;
    uint8_t flags;
    uint16_t seq;
    uint8_t payload[256];
    size_t payload_size = sizeof(payload);

    bool parsed =
        ParseUartPacket(frame.data(), frame.size(), msg_type, payload, payload_size, seq, flags);
    EXPECT_FALSE(parsed);
}

// Test: Error recovery - corrupted start byte
TEST_F(InterMcuProtocolIntegrationTest, ErrorRecoveryCorruptedStartByte) {
    HeartbeatMessage heartbeat(1000, 50, 100, 250, 200, 300);

    std::vector<uint8_t> frame =
        CreateUartPacketFromMessage(MSG_HEARTBEAT, &heartbeat, sizeof(heartbeat));
    ASSERT_FALSE(frame.empty());

    // Corrupt start byte
    frame[0] = 0x00;

    EXPECT_FALSE(ValidateUartFrame(frame.data(), frame.size()));
}

// Test: Error recovery - corrupted end byte
TEST_F(InterMcuProtocolIntegrationTest, ErrorRecoveryCorruptedEndByte) {
    HeartbeatMessage heartbeat(1000, 50, 100, 250, 200, 300);

    std::vector<uint8_t> frame =
        CreateUartPacketFromMessage(MSG_HEARTBEAT, &heartbeat, sizeof(heartbeat));
    ASSERT_FALSE(frame.empty());

    // Corrupt end byte
    frame[frame.size() - 1] = 0x00;

    EXPECT_FALSE(ValidateUartFrame(frame.data(), frame.size()));
}

// Test: Sequence number tracking through pipeline
TEST_F(InterMcuProtocolIntegrationTest, SequenceNumberTracking) {
    HeartbeatMessage heartbeat(1000, 50, 100, 250, 200, 300);

    // Create packets with different sequence numbers
    for (uint16_t seq = 0; seq < 10; seq++) {
        std::vector<uint8_t> frame =
            CreateUartPacketFromMessage(MSG_HEARTBEAT, &heartbeat, sizeof(heartbeat), seq);
        ASSERT_FALSE(frame.empty());

        uint8_t parsed_type;
        uint8_t parsed_flags;
        uint16_t parsed_seq;
        uint8_t payload[256];
        size_t payload_size = sizeof(payload);

        bool parsed = ParseUartPacket(frame.data(),
                                      frame.size(),
                                      parsed_type,
                                      payload,
                                      payload_size,
                                      parsed_seq,
                                      parsed_flags);
        ASSERT_TRUE(parsed);
        EXPECT_EQ(parsed_seq, seq);
    }
}

// Test: ACK flag handling
TEST_F(InterMcuProtocolIntegrationTest, AckFlagHandling) {
    HeartbeatMessage heartbeat(1000, 50, 100, 250, 200, 300);

    // Create packet with ACK flag
    std::vector<uint8_t> frame =
        CreateUartPacketFromMessage(MSG_HEARTBEAT, &heartbeat, sizeof(heartbeat), 0, UART_FLAG_ACK);
    ASSERT_FALSE(frame.empty());

    uint8_t parsed_type;
    uint8_t parsed_flags;
    uint16_t parsed_seq;
    uint8_t payload[256];
    size_t payload_size = sizeof(payload);

    bool parsed = ParseUartPacket(
        frame.data(), frame.size(), parsed_type, payload, payload_size, parsed_seq, parsed_flags);
    ASSERT_TRUE(parsed);
    EXPECT_TRUE(parsed_flags & UART_FLAG_ACK);
}

// Test: NACK flag handling
TEST_F(InterMcuProtocolIntegrationTest, NackFlagHandling) {
    HeartbeatMessage heartbeat(1000, 50, 100, 250, 200, 300);

    // Create packet with NACK flag
    std::vector<uint8_t> frame = CreateUartPacketFromMessage(
        MSG_HEARTBEAT, &heartbeat, sizeof(heartbeat), 0, UART_FLAG_NACK);
    ASSERT_FALSE(frame.empty());

    uint8_t parsed_type;
    uint8_t parsed_flags;
    uint16_t parsed_seq;
    uint8_t payload[256];
    size_t payload_size = sizeof(payload);

    bool parsed = ParseUartPacket(
        frame.data(), frame.size(), parsed_type, payload, payload_size, parsed_seq, parsed_flags);
    ASSERT_TRUE(parsed);
    EXPECT_TRUE(parsed_flags & UART_FLAG_NACK);
}

// Test: Multiple messages in sequence (simulating continuous communication)
TEST_F(InterMcuProtocolIntegrationTest, MultipleMessagesInSequence) {
    // Send multiple heartbeat messages
    for (int i = 0; i < 5; i++) {
        HeartbeatMessage heartbeat(static_cast<uint32_t>(1000 + (i * 100)),
                                   static_cast<uint32_t>(50 + i),
                                   static_cast<uint32_t>(100 + i),
                                   250,
                                   200,
                                   300);

        bool success = TestFullPipeline(MSG_HEARTBEAT, &heartbeat, sizeof(heartbeat));
        EXPECT_TRUE(success);
    }

    // Send mixed message types
    ControlChangeMessage control(0x01, 0x00, 0x1000);
    EXPECT_TRUE(TestFullPipeline(MSG_CONTROL_CHANGE, &control, sizeof(control)));

    MeterPushMessage meter(0x4000, 0x4000, 0x7FFF, 0x7FFF);
    EXPECT_TRUE(TestFullPipeline(MSG_METER_PUSH, &meter, sizeof(meter)));
}

// Test: Maximum payload size handling
TEST_F(InterMcuProtocolIntegrationTest, MaximumPayloadSize) {
    // Create payload at maximum size
    std::vector<uint8_t> large_payload(UART_MAX_PAYLOAD);
    for (size_t i = 0; i < large_payload.size(); i++) {
        large_payload[i] = static_cast<uint8_t>(i & 0xFF);
    }

    bool success = TestFullPipeline(MSG_BROWSE_RESP, large_payload.data(), large_payload.size());
    EXPECT_TRUE(success);

    // Verify data integrity
    std::vector<uint8_t> frame =
        CreateUartPacketFromMessage(MSG_BROWSE_RESP, large_payload.data(), large_payload.size());
    EXPECT_TRUE(ValidateUartFrame(frame.data(), frame.size()));

    uint8_t parsed_type;
    uint8_t parsed_flags;
    uint16_t parsed_seq;
    std::vector<uint8_t> parsed_payload(UART_MAX_PAYLOAD);
    size_t parsed_size = parsed_payload.size();

    bool parsed = ParseUartPacket(frame.data(),
                                  frame.size(),
                                  parsed_type,
                                  parsed_payload.data(),
                                  parsed_size,
                                  parsed_seq,
                                  parsed_flags);
    ASSERT_TRUE(parsed);
    EXPECT_EQ(parsed_size, large_payload.size());
    EXPECT_EQ(memcmp(parsed_payload.data(), large_payload.data(), large_payload.size()), 0);
}

// Test: Empty payload handling
TEST_F(InterMcuProtocolIntegrationTest, EmptyPayloadHandling) {
    // Some message types can have empty payloads
    bool success = TestFullPipeline(MSG_SYNC, nullptr, 0);
    EXPECT_TRUE(success);

    std::vector<uint8_t> frame = CreateUartPacketFromMessage(MSG_SYNC, nullptr, 0);
    EXPECT_TRUE(ValidateUartFrame(frame.data(), frame.size()));
    EXPECT_EQ(frame.size(), UART_FRAME_OVERHEAD);  // Should only have overhead
}
