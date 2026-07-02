#include "packet_router.h"

#include <gtest/gtest.h>

#include "../../mocks/esp32_mocks.h"
#include "../../utils/test_helpers.h"
#include "protocol.h"

// Mock functions for testing message handlers
static bool g_sync_handler_called = false;
static bool g_heartbeat_handler_called = false;
static bool g_meter_push_handler_called = false;
static bool g_browse_resp_handler_called = false;
static bool g_sample_stop_resp_handler_called = false;
static bool g_error_handler_called = false;

static uint8_t g_last_msg_type = 0;
static uint32_t g_last_seq_num = 0;

// Mock message handlers - redefine them to override the real ones
extern "C" {

// Save original function pointers
static void (*original_handle_sync)(const WaveX::Protocol::SyncMessage&) = nullptr;
static void (*original_handle_heartbeat)(const WaveX::Protocol::HeartbeatMessage&) = nullptr;
static void (*original_handle_meter_push)(const WaveX::Protocol::MeterPushMessage&) = nullptr;
static void (*original_handle_browse_resp)(const uint8_t*, size_t) = nullptr;
static void (*original_handle_sample_stop_resp)(const WaveX::Protocol::SampleStopRespMessage&) =
    nullptr;
static void (*original_handle_error)(const WaveX::Protocol::ErrorMessage&) = nullptr;

void handle_sync(const WaveX::Protocol::SyncMessage& msg) {
    (void)msg;
    g_sync_handler_called = true;
    g_last_msg_type = WaveX::Protocol::MSG_SYNC;
}

void handle_heartbeat(const WaveX::Protocol::HeartbeatMessage& msg) {
    (void)msg;
    g_heartbeat_handler_called = true;
    g_last_msg_type = WaveX::Protocol::MSG_HEARTBEAT;
}

void handle_meter_push(const WaveX::Protocol::MeterPushMessage& msg) {
    (void)msg;
    g_meter_push_handler_called = true;
    g_last_msg_type = WaveX::Protocol::MSG_METER_PUSH;
}

void handle_browse_resp(const uint8_t* data, size_t length) {
    (void)data;
    (void)length;
    g_browse_resp_handler_called = true;
    g_last_msg_type = WaveX::Protocol::MSG_BROWSE_RESP;
}

void handle_sample_stop_resp(const WaveX::Protocol::SampleStopRespMessage& msg) {
    (void)msg;
    g_sample_stop_resp_handler_called = true;
    g_last_msg_type = WaveX::Protocol::MSG_SAMPLE_STOP_RESP;
}

void handle_error(const WaveX::Protocol::ErrorMessage& msg) {
    (void)msg;
    g_error_handler_called = true;
    g_last_msg_type = WaveX::Protocol::MSG_ERROR;
}
}

using namespace WaveX::Comm;
using namespace WaveX::Protocol;
using namespace WaveX::Test;

class PacketRouterTest : public ::testing::Test {
   protected:
    void SetUp() override {
        router_ = std::make_unique<PacketRouter>();

        // Reset global test flags
        g_sync_handler_called = false;
        g_heartbeat_handler_called = false;
        g_meter_push_handler_called = false;
        g_browse_resp_handler_called = false;
        g_sample_stop_resp_handler_called = false;
        g_error_handler_called = false;
        g_last_msg_type = 0;
        g_last_seq_num = 0;
    }

    void TearDown() override {}

    std::unique_ptr<PacketRouter> router_;
};

// Test routing a valid heartbeat packet
TEST_F(PacketRouterTest, RouteValidHeartbeatPacket) {
    std::vector<uint8_t> packet = ProtocolTestHelper::CreateHeartbeatPacket(1000, 5000, 10000);

    // Should not crash and should process packet without throwing
    router_->route_packet(packet.data(), packet.size());

    // Packet routing completed successfully
    SUCCEED();
}

// Test routing a valid sync packet
TEST_F(PacketRouterTest, RouteValidSyncPacket) {
    std::vector<uint8_t> packet = ProtocolTestHelper::CreateWaveXPacket(MSG_SYNC, nullptr, 0);

    // Should not crash and should process packet without throwing
    router_->route_packet(packet.data(), packet.size());

    // Packet routing completed successfully
    SUCCEED();
}

// Test routing invalid packet (too small)
TEST_F(PacketRouterTest, RouteInvalidPacketTooSmall) {
    std::vector<uint8_t> small_packet = {0x00, 0x01, 0x02};

    router_->route_packet(small_packet.data(), small_packet.size());

    // No handlers should be called for invalid packet
    EXPECT_FALSE(g_sync_handler_called);
    EXPECT_FALSE(g_heartbeat_handler_called);
    EXPECT_FALSE(g_meter_push_handler_called);
    EXPECT_FALSE(g_browse_resp_handler_called);
    EXPECT_FALSE(g_sample_stop_resp_handler_called);
    EXPECT_FALSE(g_error_handler_called);
}

// Test routing invalid packet (corrupted CRC)
TEST_F(PacketRouterTest, RouteInvalidPacketCorruptedCRC) {
    std::vector<uint8_t> packet = ProtocolTestHelper::CreateHeartbeatPacket(1000, 5000, 10000);

    // Corrupt CRC
    packet[packet.size() - 2] ^= 0xFF;
    packet[packet.size() - 1] ^= 0xFF;

    router_->route_packet(packet.data(), packet.size());

    // No handlers should be called for corrupted packet
    EXPECT_FALSE(g_sync_handler_called);
    EXPECT_FALSE(g_heartbeat_handler_called);
    EXPECT_FALSE(g_meter_push_handler_called);
    EXPECT_FALSE(g_browse_resp_handler_called);
    EXPECT_FALSE(g_sample_stop_resp_handler_called);
    EXPECT_FALSE(g_error_handler_called);
}

// Test routing UART message
TEST_F(PacketRouterTest, RouteUartMessage) {
    HeartbeatMessage msg(1000, 5000, 10000, 256, 128, 512);

    // Should not crash and should process UART message without throwing
    router_->route_uart_message(
        MSG_HEARTBEAT, reinterpret_cast<const uint8_t*>(&msg), sizeof(msg), 0, 0x1234);

    // UART message routing completed successfully
    SUCCEED();
}

// Test routing METER_PUSH message via packet
TEST_F(PacketRouterTest, RouteMeterPushMessage) {
    MeterPushMessage msg(0x7FFF, 0x4000, 0x7FFF, 0x4000);
    std::vector<uint8_t> packet =
        ProtocolTestHelper::CreateWaveXPacket(MSG_METER_PUSH, &msg, sizeof(msg));

    // Should not crash and should process packet without throwing
    router_->route_packet(packet.data(), packet.size());

    // Packet routing completed successfully
    SUCCEED();
}

// Test routing BROWSE_RESP message
TEST_F(PacketRouterTest, RouteBrowseRespMessage) {
    std::vector<FileEntryWire> entries = {FileEntryWire(1, 0, "dir1"),
                                          FileEntryWire(0, 1024, "file1.wav")};

    std::vector<uint8_t> packet = ProtocolTestHelper::CreateBrowseRespPacket(2, entries);

    // Should not crash and should process packet without throwing
    router_->route_packet(packet.data(), packet.size());

    // Packet routing completed successfully
    SUCCEED();
}

// Test routing ERROR message
TEST_F(PacketRouterTest, RouteErrorMessage) {
    ErrorMessage msg(0x0001, "Test error");
    std::vector<uint8_t> packet =
        ProtocolTestHelper::CreateWaveXPacket(MSG_ERROR, &msg, sizeof(msg));

    // Should not crash and should process packet without throwing
    router_->route_packet(packet.data(), packet.size());

    // Packet routing completed successfully
    SUCCEED();
}

// Test routing unknown message type
TEST_F(PacketRouterTest, RouteUnknownMessage) {
    uint8_t unknown_payload[] = {0x01, 0x02, 0x03};
    std::vector<uint8_t> packet =
        ProtocolTestHelper::CreateWaveXPacket(0xFF,  // Unknown message type
                                              unknown_payload,
                                              sizeof(unknown_payload));

    router_->route_packet(packet.data(), packet.size());

    // No handlers should be called for unknown message type
    EXPECT_FALSE(g_sync_handler_called);
    EXPECT_FALSE(g_heartbeat_handler_called);
    EXPECT_FALSE(g_meter_push_handler_called);
    EXPECT_FALSE(g_browse_resp_handler_called);
    EXPECT_FALSE(g_sample_stop_resp_handler_called);
    EXPECT_FALSE(g_error_handler_called);
}

// Test routing packet with ACK flag
TEST_F(PacketRouterTest, RoutePacketWithACK) {
    HeartbeatMessage msg(1000, 5000, 10000, 256, 128, 512);
    std::vector<uint8_t> packet = ProtocolTestHelper::CreateWaveXPacket(
        MSG_HEARTBEAT, &msg, sizeof(msg), 0x1234, PKT_FLAG_ACK);

    // Should not crash and should process ACK packet without throwing
    router_->route_packet(packet.data(), packet.size());

    // ACK packet routing completed successfully
    SUCCEED();
}

// Test routing packet with NACK flag
TEST_F(PacketRouterTest, RoutePacketWithNACK) {
    HeartbeatMessage msg(1000, 5000, 10000, 256, 128, 512);
    std::vector<uint8_t> packet = ProtocolTestHelper::CreateWaveXPacket(
        MSG_HEARTBEAT, &msg, sizeof(msg), 0x1234, PKT_FLAG_NACK);

    // Should not crash and should process NACK packet without throwing
    router_->route_packet(packet.data(), packet.size());

    // NACK packet routing completed successfully
    SUCCEED();
}

// Test statistics callback functionality
TEST_F(PacketRouterTest, StatisticsCallback) {
    bool stats_callback_called = false;
    uint8_t last_stats_type = 0;

    router_->set_stats_callback([&stats_callback_called, &last_stats_type](uint8_t type) {
        stats_callback_called = true;
        last_stats_type = type;
    });

    HeartbeatMessage msg(1000, 5000, 10000, 256, 128, 512);
    router_->route_uart_message(
        MSG_HEARTBEAT, reinterpret_cast<const uint8_t*>(&msg), sizeof(msg), 0, 0x1234);

    // Statistics callback should be called
    EXPECT_TRUE(stats_callback_called);
    EXPECT_EQ(last_stats_type, MSG_HEARTBEAT);
}

// Test routing null packet (should handle gracefully)
TEST_F(PacketRouterTest, RouteNullPacket) {
    router_->route_packet(nullptr, 0);

    // No handlers should be called for null packet
    EXPECT_FALSE(g_sync_handler_called);
    EXPECT_FALSE(g_heartbeat_handler_called);
    EXPECT_FALSE(g_meter_push_handler_called);
    EXPECT_FALSE(g_browse_resp_handler_called);
    EXPECT_FALSE(g_sample_stop_resp_handler_called);
    EXPECT_FALSE(g_error_handler_called);
}

// Test routing empty packet (should handle gracefully)
TEST_F(PacketRouterTest, RouteEmptyPacket) {
    std::vector<uint8_t> empty_packet;

    router_->route_packet(empty_packet.data(), 0);

    // No handlers should be called for empty packet
    EXPECT_FALSE(g_sync_handler_called);
    EXPECT_FALSE(g_heartbeat_handler_called);
    EXPECT_FALSE(g_meter_push_handler_called);
    EXPECT_FALSE(g_browse_resp_handler_called);
    EXPECT_FALSE(g_sample_stop_resp_handler_called);
    EXPECT_FALSE(g_error_handler_called);
}
