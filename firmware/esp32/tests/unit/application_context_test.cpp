/**
 * @file application_context_test.cpp
 * @brief Unit tests for ApplicationContext wiring.
 *
 * ApplicationContext's one job is to own the components and wire them to the
 * same instances. These tests assert observable wiring (data written through
 * one accessor is readable through another), not merely that references
 * exist - a reference returned by getX() can never be null, so checking its
 * address proves nothing.
 */

#include "application_context.h"

#include <gtest/gtest.h>

#include "../mocks/esp32_mocks.h"
#include "../utils/test_helpers.h"

#include <vector>

namespace {

using WaveX::Test::GetInterMcuCapture;
using WaveX::Test::ProtocolTestHelper;
using WaveX::Test::ResetInterMcuCapture;

class ApplicationContextTest : public ::testing::Test {
   protected:
    void SetUp() override {
        ResetInterMcuCapture();
        context = new WaveX::ApplicationContext();
    }

    void TearDown() override {
        delete context;
        context = nullptr;
    }

    WaveX::ApplicationContext* context = nullptr;
};

// Accessors must hand back the same instances on every call - components are
// owned, not created per accessor.
TEST_F(ApplicationContextTest, GettersReturnStableInstances) {
    auto& stats = context->getStatistics();
    auto& router = context->getPacketRouter();
    auto& comm = context->getCommInterface();

    EXPECT_EQ(&context->getStatistics(), &stats);
    EXPECT_EQ(&context->getPacketRouter(), &router);
    EXPECT_EQ(&context->getCommInterface(), &comm);
}

// Registration and delivery must meet at the same injected statistics owner.
TEST_F(ApplicationContextTest, ListenersRegisteredViaCommInterfaceFire) {
    bool mounted = false;
    context->getCommInterface().setStorageStatusListener(
        [](bool value, void* data) { *static_cast<bool*>(data) = value; }, &mounted);
    context->getStatistics().invoke_storage_status_callback(true);
    EXPECT_TRUE(mounted);
    context->getCommInterface().setStorageStatusListener(nullptr, nullptr);
    context->getStatistics().invoke_storage_status_callback(false);
    EXPECT_TRUE(mounted);
}

// The context's router must actually route: a real heartbeat packet through
// getPacketRouter() must reach the inter_mcu boundary with its content.
TEST_F(ApplicationContextTest, RouterRoutesRealPacketsToHandlers) {
    std::vector<uint8_t> packet = ProtocolTestHelper::CreateHeartbeatPacket(1000, 5000, 10000);
    ASSERT_FALSE(packet.empty());

    context->getPacketRouter().route_packet(packet.data(), packet.size());

    const auto& cap = GetInterMcuCapture();
    ASSERT_EQ(cap.heartbeat_calls, 1);
    EXPECT_EQ(cap.hb_uptime_ms, 1000u);
    EXPECT_EQ(cap.hb_rx_total, 5000u);
    EXPECT_EQ(cap.hb_loop_counter, 10000u);
}

TEST_F(ApplicationContextTest, RouterDropsGarbageWithoutDispatching) {
    const uint8_t garbage[] = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07};
    context->getPacketRouter().route_packet(garbage, sizeof(garbage));

    const auto& cap = GetInterMcuCapture();
    EXPECT_EQ(cap.heartbeat_calls, 0);
    EXPECT_EQ(cap.meter_calls, 0);
    EXPECT_EQ(cap.browse_resp_calls, 0);
}

TEST_F(ApplicationContextTest, MultipleInstancesAreIndependent) {
    WaveX::ApplicationContext context2;

    auto& stats1 = context->getStatistics();
    auto& stats2 = context2.getStatistics();
    ASSERT_NE(&stats1, &stats2);

    stats1.increment_packet_stat(0x00);
    stats1.increment_packet_stat(0x00);

    EXPECT_EQ(stats1.get_total_packet_count(), 2u);
    EXPECT_EQ(stats2.get_total_packet_count(), 0u);
}

}  // namespace
