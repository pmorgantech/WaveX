/**
 * @file statistics_test.cpp
 * @brief Unit tests for StatisticsManager class
 */

#include "comm/statistics.h"

#include <gtest/gtest.h>

// Mock FreeRTOS functions for unit testing
#ifdef WAVEX_TEST_BUILD
extern "C" {
// Undefine FreeRTOS macros to avoid conflicts
#undef taskENTER_CRITICAL
#undef taskEXIT_CRITICAL

// Mock critical section functions
void taskENTER_CRITICAL(portMUX_TYPE* mux) {
    (void)mux;  // No-op for tests
}

void taskEXIT_CRITICAL(portMUX_TYPE* mux) {
    (void)mux;  // No-op for tests
}
}
#endif

namespace {

// Static callback variables for testing
static float g_callback_rms_left = 0.0f;
static float g_callback_rms_right = 0.0f;
static float g_callback_peak_left = 0.0f;
static float g_callback_peak_right = 0.0f;
static bool g_callback_called = false;
static uint8_t g_callback_state = 0;
static uint32_t g_callback_sample_rate = 0;
static uint8_t g_callback_channels = 0;
static uint32_t g_callback_frames_played = 0;

// Static callback functions
static void meter_callback(
    float rms_left, float rms_right, float peak_left, float peak_right, void* user_data) {
    g_callback_rms_left = rms_left;
    g_callback_rms_right = rms_right;
    g_callback_peak_left = peak_left;
    g_callback_peak_right = peak_right;
    if (user_data) {
        *static_cast<float*>(user_data) = rms_left;
    }
}

static void sample_status_callback(uint8_t state,
                                   uint32_t sample_rate,
                                   uint8_t channels,
                                   uint32_t frames_played,
                                   void* user_data) {
    g_callback_state = state;
    g_callback_sample_rate = sample_rate;
    g_callback_channels = channels;
    g_callback_frames_played = frames_played;
    if (user_data) {
        *static_cast<bool*>(user_data) = true;
    }
}

static void browse_response_callback(const uint8_t* data, size_t length, void* user_data) {
    if (user_data) {
        *static_cast<bool*>(user_data) = true;
    }
    // Verify data
    if (length == 4 && data[0] == 0x01 && data[1] == 0x02 && data[2] == 0x03 && data[3] == 0x04) {
        // Data is correct
    }
}

// Test fixture for StatisticsManager
class StatisticsManagerTest : public ::testing::Test {
   protected:
    void SetUp() override {
        // Create a fresh StatisticsManager for each test
        stats = new StatisticsManager();

        // Reset static callback variables
        g_callback_rms_left = 0.0f;
        g_callback_rms_right = 0.0f;
        g_callback_peak_left = 0.0f;
        g_callback_peak_right = 0.0f;
        g_callback_called = false;
        g_callback_state = 0;
        g_callback_sample_rate = 0;
        g_callback_channels = 0;
        g_callback_frames_played = 0;
    }

    void TearDown() override {
        delete stats;
        stats = nullptr;
    }

    StatisticsManager* stats = nullptr;
};

// Test packet statistics incrementing
TEST_F(StatisticsManagerTest, IncrementPacketStats) {
    wavex_packet_stats_t packet_stats;

    // Initial state should be zero
    stats->get_packet_stats(&packet_stats);
    EXPECT_EQ(packet_stats.total_packets, 0);
    EXPECT_EQ(packet_stats.sync_packets, 0);
    EXPECT_EQ(packet_stats.meter_push_packets, 0);

    // Increment sync packet
    stats->increment_packet_stat(0x00);  // SYNC packet type
    stats->get_packet_stats(&packet_stats);
    EXPECT_EQ(packet_stats.sync_packets, 1);
    EXPECT_EQ(packet_stats.total_packets, 1);

    // Increment meter push packet
    stats->increment_packet_stat(0x10);  // METER_PUSH packet type
    stats->get_packet_stats(&packet_stats);
    EXPECT_EQ(packet_stats.meter_push_packets, 1);
    EXPECT_EQ(packet_stats.total_packets, 2);
}

// Test invalid packet incrementing
TEST_F(StatisticsManagerTest, IncrementInvalidPacket) {
    wavex_packet_stats_t packet_stats;

    // Initial state
    stats->get_packet_stats(&packet_stats);
    EXPECT_EQ(packet_stats.invalid_packets, 0);

    // Increment invalid packet
    stats->increment_invalid_packet();
    stats->get_packet_stats(&packet_stats);
    EXPECT_EQ(packet_stats.invalid_packets, 1);
    EXPECT_EQ(packet_stats.total_packets, 1);
}

// Test packet stats reset
TEST_F(StatisticsManagerTest, ResetPacketStats) {
    wavex_packet_stats_t packet_stats;

    // Add some data
    stats->increment_packet_stat(0x01);
    stats->increment_invalid_packet();
    stats->get_packet_stats(&packet_stats);
    EXPECT_EQ(packet_stats.total_packets, 2);

    // Reset
    stats->reset_packet_stats();
    stats->get_packet_stats(&packet_stats);
    EXPECT_EQ(packet_stats.total_packets, 0);
    EXPECT_EQ(packet_stats.sync_packets, 0);
    EXPECT_EQ(packet_stats.invalid_packets, 0);
}

// Test packet summary
TEST_F(StatisticsManagerTest, GetPacketSummary) {
    wavex_packet_summary_t summary;

    // Add various packet types
    stats->increment_packet_stat(0x00);  // SYNC (not a control packet)
    stats->increment_packet_stat(0x10);  // METER_PUSH
    stats->increment_packet_stat(0x12);  // HEARTBEAT
    stats->increment_packet_stat(0x01);  // CONTROL_CHANGE (counts as control packet)
    stats->increment_packet_stat(0x02);  // NOTE_ON (counts as control packet)
    stats->increment_invalid_packet();

    stats->get_packet_summary(&summary);
    EXPECT_EQ(summary.total_packets, 6);
    EXPECT_EQ(summary.meter_packets, 1);
    EXPECT_EQ(summary.heartbeat_packets, 1);
    EXPECT_EQ(summary.control_packets, 2);  // CONTROL_CHANGE + NOTE_ON
    EXPECT_EQ(summary.invalid_packets, 1);
}

// Test TX statistics
TEST_F(StatisticsManagerTest, TxStats) {
    wavex_tx_stats_t tx_stats;

    // Initial state
    stats->get_tx_stats(&tx_stats);
    EXPECT_EQ(tx_stats.total_messages_sent, 0);
    EXPECT_EQ(tx_stats.ping_messages_sent, 0);

    // Increment ping message
    stats->increment_tx_message(0x01);  // PING message type
    stats->get_tx_stats(&tx_stats);
    EXPECT_EQ(tx_stats.ping_messages_sent, 1);
    EXPECT_EQ(tx_stats.total_messages_sent, 1);
}

// Test backend heartbeat
TEST_F(StatisticsManagerTest, BackendHeartbeat) {
    uint32_t uptime_ms, rx_total, loop_counter, last_rx_ms;
    float cpu_usage_percent;
    bool valid;

    // Initial state should be invalid
    stats->get_backend_heartbeat(
        &uptime_ms, &rx_total, &loop_counter, &last_rx_ms, &cpu_usage_percent, &valid);
    EXPECT_FALSE(valid);

    // Update heartbeat
    stats->update_backend_heartbeat(1000, 500, 100, 25.5f);
    stats->get_backend_heartbeat(
        &uptime_ms, &rx_total, &loop_counter, &last_rx_ms, &cpu_usage_percent, &valid);

    EXPECT_TRUE(valid);
    EXPECT_EQ(uptime_ms, 1000);
    EXPECT_EQ(rx_total, 500);
    EXPECT_EQ(loop_counter, 100);
    EXPECT_FLOAT_EQ(cpu_usage_percent, 25.5f);
}

// Test detailed backend heartbeat
TEST_F(StatisticsManagerTest, BackendHeartbeatDetailed) {
    uint32_t uptime_ms, rx_total, loop_counter, last_rx_ms;
    float cpu_avg_percent, cpu_min_percent, cpu_max_percent;
    bool valid;

    // Update with detailed metrics
    stats->update_backend_heartbeat_detailed(2000, 1000, 200, 30.0f, 20.0f, 40.0f);
    stats->get_backend_heartbeat_detailed(&uptime_ms,
                                          &rx_total,
                                          &loop_counter,
                                          &last_rx_ms,
                                          &cpu_avg_percent,
                                          &cpu_min_percent,
                                          &cpu_max_percent,
                                          &valid);

    EXPECT_TRUE(valid);
    EXPECT_EQ(uptime_ms, 2000);
    EXPECT_EQ(rx_total, 1000);
    EXPECT_EQ(loop_counter, 200);
    EXPECT_FLOAT_EQ(cpu_avg_percent, 30.0f);
    EXPECT_FLOAT_EQ(cpu_min_percent, 20.0f);
    EXPECT_FLOAT_EQ(cpu_max_percent, 40.0f);
}

// Test meter data
TEST_F(StatisticsManagerTest, MeterData) {
    wavex_meter_data_t meter_data;

    // Initial state
    stats->get_meter_data(&meter_data);
    EXPECT_FALSE(meter_data.valid);

    // Update meter data
    stats->update_meter_data(0.5f, 0.3f, 0.8f, 0.6f);
    stats->get_meter_data(&meter_data);

    EXPECT_TRUE(meter_data.valid);
    EXPECT_FLOAT_EQ(meter_data.rms_left, 0.5f);
    EXPECT_FLOAT_EQ(meter_data.rms_right, 0.3f);
    EXPECT_FLOAT_EQ(meter_data.peak_left, 0.8f);
    EXPECT_FLOAT_EQ(meter_data.peak_right, 0.6f);
    // Note: last_update_ms is 0 in test environment (non-ESP_PLATFORM)
}

// Test meter callback
TEST_F(StatisticsManagerTest, MeterCallback) {
    float user_data = 42.0f;

    // Set callback
    stats->set_meter_callback(meter_callback, &user_data);

    // Update meter data (should trigger callback)
    stats->update_meter_data(0.7f, 0.4f, 0.9f, 0.7f);

    EXPECT_FLOAT_EQ(g_callback_rms_left, 0.7f);
    EXPECT_FLOAT_EQ(g_callback_rms_right, 0.4f);
    EXPECT_FLOAT_EQ(g_callback_peak_left, 0.9f);
    EXPECT_FLOAT_EQ(g_callback_peak_right, 0.7f);
    EXPECT_FLOAT_EQ(user_data, 0.7f);  // Should be modified by callback
}

// Test browse response callback
TEST_F(StatisticsManagerTest, BrowseResponseCallback) {
    const uint8_t test_data[] = {0x01, 0x02, 0x03, 0x04};
    size_t test_length = sizeof(test_data);
    bool callback_called = false;

    // Set callback
    stats->set_browse_resp_callback(browse_response_callback, &callback_called);

    // Invoke callback
    stats->invoke_browse_resp_callback(test_data, test_length);

    EXPECT_TRUE(callback_called);
}

// Test sample status callback
TEST_F(StatisticsManagerTest, SampleStatusCallback) {
    bool callback_called = false;

    // Set callback
    stats->set_sample_status_callback(sample_status_callback, &callback_called);

    // Invoke callback
    stats->invoke_sample_status_callback(1, 44100, 2, 1000);

    EXPECT_TRUE(callback_called);
    EXPECT_EQ(g_callback_state, 1);
    EXPECT_EQ(g_callback_sample_rate, 44100);
    EXPECT_EQ(g_callback_channels, 2);
    EXPECT_EQ(g_callback_frames_played, 1000);
}

// Test packet type name formatting
TEST_F(StatisticsManagerTest, FormatPacketStats) {
    char buffer[1024];

    // Add some packet data
    stats->increment_packet_stat(0x00);  // SYNC
    stats->increment_packet_stat(0x10);  // METER_PUSH
    stats->increment_invalid_packet();

    int result = stats->format_packet_stats(buffer, sizeof(buffer));
    EXPECT_GT(result, 0);
    EXPECT_NE(strstr(buffer, "SYNC"), nullptr);
    EXPECT_NE(strstr(buffer, "METER="), nullptr);
    EXPECT_NE(strstr(buffer, "Invalid"), nullptr);
}

// Test meter packet count
TEST_F(StatisticsManagerTest, GetMeterPacketCount) {
    EXPECT_EQ(stats->get_meter_packet_count(), 0);

    stats->increment_packet_stat(0x10);  // METER_PUSH
    EXPECT_EQ(stats->get_meter_packet_count(), 1);

    stats->increment_packet_stat(0x10);  // Another METER_PUSH
    EXPECT_EQ(stats->get_meter_packet_count(), 2);
}

// Test total packet count
TEST_F(StatisticsManagerTest, GetTotalPacketCount) {
    EXPECT_EQ(stats->get_total_packet_count(), 0);

    stats->increment_packet_stat(0x01);
    EXPECT_EQ(stats->get_total_packet_count(), 1);

    stats->increment_invalid_packet();
    EXPECT_EQ(stats->get_total_packet_count(), 2);
}

}  // namespace
