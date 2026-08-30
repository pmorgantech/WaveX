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

// The CommInterface must be constructed over the SAME StatisticsManager the
// context exposes: data pushed through getStatistics() must be readable
// through getCommInterface(). This is the wiring the whole UI depends on.
TEST_F(ApplicationContextTest, CommInterfaceSharesTheContextsStatisticsManager) {
    context->getStatistics().update_backend_heartbeat(4242, 17, 99, 33.5f);

    wavex_backend_heartbeat_t hb;
    context->getCommInterface().getBackendHeartbeat(&hb);
    ASSERT_TRUE(hb.valid);
    EXPECT_EQ(hb.uptime_ms, 4242u);
    EXPECT_EQ(hb.rx_total, 17u);
    EXPECT_EQ(hb.loop_counter, 99u);
    EXPECT_FLOAT_EQ(hb.cpu_usage_percent, 33.5f);

    context->getStatistics().increment_packet_stat(0x10);
    wavex_packet_stats_t s;
    context->getCommInterface().getPacketStats(&s);
    EXPECT_EQ(s.meter_push_packets, 1u);
}

// Listener registration through the interface and invocation through the
// statistics manager must meet at the same slot.
TEST_F(ApplicationContextTest, ListenersRegisteredViaCommInterfaceFire) {
    static float s_last_rms;
    s_last_rms = -1.0f;

    context->getCommInterface().setMeterListener(
        [](float rms_left, float rms_right, float peak_left, float peak_right, void* user_data) {
            (void)rms_right;
            (void)peak_left;
            (void)peak_right;
            (void)user_data;
            s_last_rms = rms_left;
        },
        nullptr);

    context->getStatistics().update_meter_data(0.75f, 0.5f, 0.9f, 0.8f);
    EXPECT_FLOAT_EQ(s_last_rms, 0.75f);
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
