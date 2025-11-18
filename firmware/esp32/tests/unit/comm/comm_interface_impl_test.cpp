/**
 * @file comm_interface_impl_test.cpp
 * @brief Unit tests for CommInterfaceImpl class
 */

#include "comm/comm_interface_impl.h"

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
static bool g_comm_callback_called = false;

// Static callback functions
static void comm_meter_callback(
    float rms_left, float rms_right, float peak_left, float peak_right, void* user_data) {
    (void)rms_left;
    (void)rms_right;
    (void)peak_left;
    (void)peak_right;
    if (user_data) {
        *static_cast<bool*>(user_data) = true;
    }
    g_comm_callback_called = true;
}

static void comm_browse_callback(const uint8_t* data, size_t length, void* user_data) {
    (void)data;
    (void)length;
    if (user_data) {
        *static_cast<bool*>(user_data) = true;
    }
}

static void comm_sample_status_callback(uint8_t state,
                                        uint32_t sample_rate,
                                        uint8_t channels,
                                        uint32_t frames_played,
                                        void* user_data) {
    (void)state;
    (void)sample_rate;
    (void)channels;
    (void)frames_played;
    if (user_data) {
        *static_cast<bool*>(user_data) = true;
    }
}

// Test fixture for CommInterfaceImpl
class CommInterfaceImplTest : public ::testing::Test {
   protected:
    void SetUp() override {
        // Create a fresh StatisticsManager and CommInterfaceImpl for each test
        stats = new StatisticsManager();
        comm = new WaveX::Comm::CommInterfaceImpl(*stats);

        // Reset static callback variables
        g_comm_callback_called = false;
    }

    void TearDown() override {
        delete comm;
        comm = nullptr;
        delete stats;
        stats = nullptr;
    }

    StatisticsManager* stats = nullptr;
    WaveX::Comm::CommInterfaceImpl* comm = nullptr;
};

// Test construction
TEST_F(CommInterfaceImplTest, Constructor_ValidConstruction) {
    // Construction should succeed without throwing
    SUCCEED();
}

// Test meter listener operations
TEST_F(CommInterfaceImplTest, SetMeterListener) {
    bool callback_called = false;

    // Set callback through CommInterfaceImpl
    comm->setMeterListener(comm_meter_callback, &callback_called);

    // Update meter data through StatisticsManager to trigger callback
    stats->update_meter_data(0.5f, 0.3f, 0.8f, 0.6f);

    // Callback should be called when meter data is updated
    EXPECT_TRUE(callback_called);
    EXPECT_TRUE(g_comm_callback_called);
}

// Test get meter data
TEST_F(CommInterfaceImplTest, GetMeterData) {
    wavex_meter_data_t meter_data;

    // Initially should be invalid
    comm->getMeterData(&meter_data);
    EXPECT_FALSE(meter_data.valid);

    // Update meter data through StatisticsManager
    stats->update_meter_data(0.5f, 0.3f, 0.8f, 0.6f);

    // Get meter data through interface
    comm->getMeterData(&meter_data);
    EXPECT_TRUE(meter_data.valid);
    EXPECT_FLOAT_EQ(meter_data.rms_left, 0.5f);
    EXPECT_FLOAT_EQ(meter_data.rms_right, 0.3f);
    EXPECT_FLOAT_EQ(meter_data.peak_left, 0.8f);
    EXPECT_FLOAT_EQ(meter_data.peak_right, 0.6f);
}

// Test browse response listener
TEST_F(CommInterfaceImplTest, SetBrowseResponseListener) {
    bool callback_called = false;

    // Set callback
    comm->setBrowseResponseListener(comm_browse_callback, &callback_called);

    // Invoke callback through StatisticsManager
    const uint8_t test_data[] = {0x01, 0x02, 0x03};
    stats->invoke_browse_resp_callback(test_data, sizeof(test_data));

    EXPECT_TRUE(callback_called);
}

// Test sample status listener
TEST_F(CommInterfaceImplTest, SetSampleStatusListener) {
    bool callback_called = false;

    // Set callback
    comm->setSampleStatusListener(comm_sample_status_callback, &callback_called);

    // Invoke callback through StatisticsManager
    stats->invoke_sample_status_callback(1, 44100, 2, 1000);

    EXPECT_TRUE(callback_called);
}

// Test send browse request
TEST_F(CommInterfaceImplTest, SendBrowseRequest) {
    // This test would require mocking inter_mcu functions
    // For now, just ensure it doesn't crash
    esp_err_t result = comm->sendBrowseRequest("/samples", 0);

    // In test environment, this will likely fail due to missing inter_mcu
    // But it should not crash
    (void)result;  // Suppress unused variable warning
    SUCCEED();
}

// Test send sample play request
TEST_F(CommInterfaceImplTest, SendSamplePlayRequest) {
    // This test would require mocking inter_mcu functions
    // For now, just ensure it doesn't crash
    esp_err_t result = comm->sendSamplePlayRequest(42);

    // In test environment, this will likely fail due to missing inter_mcu
    // But it should not crash
    (void)result;  // Suppress unused variable warning
    SUCCEED();
}

// Test send sample stop request
TEST_F(CommInterfaceImplTest, SendSampleStopRequest) {
    // This test would require mocking inter_mcu functions
    // For now, just ensure it doesn't crash
    esp_err_t result = comm->sendSampleStopRequest();

    // In test environment, this will likely fail due to missing inter_mcu
    // But it should not crash
    (void)result;  // Suppress unused variable warning
    SUCCEED();
}

// Test get backend heartbeat
TEST_F(CommInterfaceImplTest, GetBackendHeartbeat) {
    wavex_backend_heartbeat_t hb;

    // Initially should be invalid
    comm->getBackendHeartbeat(&hb);
    EXPECT_FALSE(hb.valid);

    // Update heartbeat through StatisticsManager
    stats->update_backend_heartbeat(1000, 500, 100, 25.5f);

    // Get heartbeat through interface
    comm->getBackendHeartbeat(&hb);
    EXPECT_TRUE(hb.valid);
    EXPECT_EQ(hb.uptime_ms, 1000);
    EXPECT_EQ(hb.rx_total, 500);
    EXPECT_EQ(hb.loop_counter, 100);
    EXPECT_FLOAT_EQ(hb.cpu_usage_percent, 25.5f);
}

// Test get packet stats
TEST_F(CommInterfaceImplTest, GetPacketStats) {
    wavex_packet_stats_t packet_stats;

    // Initially should have zero packets
    comm->getPacketStats(&packet_stats);
    EXPECT_EQ(packet_stats.total_packets, 0);

    // Add some packet data through StatisticsManager
    stats->increment_packet_stat(0x00);  // SYNC
    stats->increment_packet_stat(0x10);  // METER_PUSH

    // Get packet stats through interface
    comm->getPacketStats(&packet_stats);
    EXPECT_EQ(packet_stats.total_packets, 2);
    EXPECT_EQ(packet_stats.sync_packets, 1);
    EXPECT_EQ(packet_stats.meter_push_packets, 1);
}

// Test is busy
TEST_F(CommInterfaceImplTest, IsBusy) {
    // This is a simple method that should return false in most cases
    // The implementation may be more complex in the future
    bool busy = comm->isBusy();

    // For now, expect it to return false
    EXPECT_FALSE(busy);
}

// Test null pointer handling
TEST_F(CommInterfaceImplTest, NullPointerHandling) {
    // These should not crash with null pointers
    comm->getMeterData(nullptr);
    comm->getBackendHeartbeat(nullptr);
    comm->getPacketStats(nullptr);

    SUCCEED();
}

}  // namespace
