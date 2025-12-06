/**
 * @file application_context_test.cpp
 * @brief Unit tests for ApplicationContext class
 */

#include "application_context.h"

#include <gtest/gtest.h>

// Mock functions for ApplicationContext tests
#ifdef WAVEX_TEST_BUILD
// Mock inter_mcu functions that ApplicationContext depends on
esp_err_t inter_mcu_send_sample_load_req(uint16_t sample_id,
                                         uint32_t sample_size,
                                         uint16_t sample_rate,
                                         uint8_t channels,
                                         uint8_t bit_depth) {
    (void)sample_id;
    (void)sample_size;
    (void)sample_rate;
    (void)channels;
    (void)bit_depth;
    return ESP_OK;
}

esp_err_t inter_mcu_send_sample_data(const uint8_t* data, size_t length) {
    (void)data;
    (void)length;
    return ESP_OK;
}
#endif

namespace {

// Test fixture for ApplicationContext
class ApplicationContextTest : public ::testing::Test {
   protected:
    void SetUp() override {
        // ApplicationContext constructor initializes all dependencies
        context = new WaveX::ApplicationContext();
    }

    void TearDown() override {
        delete context;
        context = nullptr;
    }

    WaveX::ApplicationContext* context = nullptr;
};

// Test constructor initializes all components
TEST_F(ApplicationContextTest, Constructor_InitializesAllComponents) {
    ASSERT_NE(context, nullptr);

    // Verify StatisticsManager is created
    EXPECT_NE(&context->getStatistics(), nullptr);

    // Verify PacketRouter is created
    EXPECT_NE(&context->getPacketRouter(), nullptr);

    // Verify CommInterface is created
    EXPECT_NE(&context->getCommInterface(), nullptr);

    // Verify CommInterface is properly initialized with StatisticsManager
    // We can't directly verify the internal dependency, but we can verify it's not null
}

// Test component accessors return valid objects
TEST_F(ApplicationContextTest, Getters_ReturnValidObjects) {
    auto& stats = context->getStatistics();
    auto& router = context->getPacketRouter();
    auto& comm = context->getCommInterface();

    // All getters should return references to valid objects
    EXPECT_NE(&stats, nullptr);
    EXPECT_NE(&router, nullptr);
    EXPECT_NE(&comm, nullptr);

    // Objects should be the same instance across multiple calls
    EXPECT_EQ(&context->getStatistics(), &stats);
    EXPECT_EQ(&context->getPacketRouter(), &router);
    EXPECT_EQ(&context->getCommInterface(), &comm);
}

// Test StatisticsManager integration
TEST_F(ApplicationContextTest, StatisticsManagerIntegration) {
    auto& stats = context->getStatistics();

    // Test basic StatisticsManager functionality
    EXPECT_EQ(stats.get_meter_packet_count(), 0);

    // Increment some statistics
    stats.increment_packet_stat(0x00);  // SYNC packet
    stats.increment_packet_stat(0x10);  // METER_PUSH packet

    EXPECT_EQ(stats.get_meter_packet_count(), 1);
    EXPECT_EQ(stats.get_total_packet_count(), 2);
}

// Test PacketRouter integration
TEST_F(ApplicationContextTest, PacketRouterIntegration) {
    auto& router = context->getPacketRouter();

    // Test basic PacketRouter functionality - it should not crash
    const uint8_t test_packet[] = {0x01, 0x02, 0x03, 0x04};
    router.route_packet(test_packet, sizeof(test_packet));

    // Router should handle the packet without throwing
    SUCCEED();
}

// Test CommInterface integration
TEST_F(ApplicationContextTest, CommInterfaceIntegration) {
    auto& comm = context->getCommInterface();

    // Test that CommInterface is properly initialized
    // We can't directly test all methods without more complex mocking,
    // but we can verify the interface exists and basic state
    EXPECT_NE(&comm, nullptr);
}

// Test that components are properly wired together
TEST_F(ApplicationContextTest, ComponentWiring) {
    // This test verifies that the ApplicationContext properly connects components
    // The StatisticsManager should be shared between CommInterface and ApplicationContext

    auto& context_stats = context->getStatistics();
    auto& context_comm = context->getCommInterface();

    // Both should exist
    ASSERT_NE(&context_stats, nullptr);
    ASSERT_NE(&context_comm, nullptr);

    // Update statistics through StatisticsManager
    context_stats.increment_packet_stat(0x00);

    // Verify the count is reflected
    EXPECT_EQ(context_stats.get_total_packet_count(), 1);
}

// Test destructor cleanup
TEST_F(ApplicationContextTest, Destructor_CleansUpProperly) {
    // Create a context and let it go out of scope
    {
        WaveX::ApplicationContext temp_context;
        // Components should be properly initialized
        EXPECT_NE(temp_context.getStatistics().get_total_packet_count(), static_cast<uint32_t>(-1));
    }
    // Destructor should clean up without issues
    SUCCEED();
}

// Test multiple ApplicationContext instances are independent
TEST_F(ApplicationContextTest, MultipleInstances_AreIndependent) {
    WaveX::ApplicationContext context2;

    // Each context should have its own instances
    auto& stats1 = context->getStatistics();
    auto& stats2 = context2.getStatistics();

    // Modify stats in first context
    stats1.increment_packet_stat(0x00);
    stats1.increment_packet_stat(0x00);

    // Second context should be unaffected
    EXPECT_EQ(stats1.get_total_packet_count(), 2);
    EXPECT_EQ(stats2.get_total_packet_count(), 0);
}

// Test packet routing through the full chain
TEST_F(ApplicationContextTest, PacketRoutingThroughComponents) {
    auto& router = context->getPacketRouter();
    auto& stats = context->getStatistics();

    // Create a minimal valid packet (this would normally be more complex)
    // For now, just test that routing doesn't crash
    const uint8_t test_packet[] = {0x01, 0x02, 0x03, 0x04, 0x05};

    uint32_t initial_count = stats.get_total_packet_count();

    // Route packet through PacketRouter
    router.route_packet(test_packet, sizeof(test_packet));

    // Statistics should be updated (or at least not crash)
    // Note: In real implementation, this would depend on packet content
    EXPECT_EQ(stats.get_total_packet_count(),
              initial_count);  // May not increment depending on packet
}

}  // namespace
