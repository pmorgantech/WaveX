/**
 * @file comm_interface_impl_test.cpp
 * @brief Unit tests for CommInterfaceImpl.
 *
 * CommInterfaceImpl is the UI's only door to the comm layer: listener
 * registration goes through the injected StatisticsManager (real instance
 * here), sends go through inter_mcu_send_* (capture mocks with configurable
 * results, so the link-down/error propagation the UI depends on is actually
 * exercised - see mocks/esp32_mocks.h).
 */

#include "comm/comm_interface_impl.h"

#include <gtest/gtest.h>

#include "../../mocks/esp32_mocks.h"

#include <cstring>

namespace {

using WaveX::Test::GetInterMcuCapture;
using WaveX::Test::ResetInterMcuCapture;

// Static callback variables for testing
static bool g_comm_callback_called = false;

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

static void comm_sample_status_callback(uint16_t sample_id,
                                        uint8_t state,
                                        uint32_t sample_rate,
                                        uint8_t channels,
                                        uint32_t frames_played,
                                        void* user_data) {
    (void)sample_id;
    (void)state;
    (void)sample_rate;
    (void)channels;
    (void)frames_played;
    if (user_data) {
        *static_cast<bool*>(user_data) = true;
    }
}

class CommInterfaceImplTest : public ::testing::Test {
   protected:
    void SetUp() override {
        ResetInterMcuCapture();
        stats = new StatisticsManager();
        comm = new WaveX::Comm::CommInterfaceImpl(*stats);
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

// A listener registered through the interface must fire when the underlying
// StatisticsManager receives data - that indirection is the class's job.
TEST_F(CommInterfaceImplTest, SetMeterListener) {
    bool callback_called = false;
    comm->setMeterListener(comm_meter_callback, &callback_called);

    stats->update_meter_data(0.5f, 0.3f, 0.8f, 0.6f);

    EXPECT_TRUE(callback_called);
    EXPECT_TRUE(g_comm_callback_called);
}

TEST_F(CommInterfaceImplTest, ClearedMeterListenerStopsFiring) {
    bool callback_called = false;
    comm->setMeterListener(comm_meter_callback, &callback_called);
    comm->setMeterListener(nullptr, nullptr);

    stats->update_meter_data(0.5f, 0.3f, 0.8f, 0.6f);

    EXPECT_FALSE(callback_called) << "listener fired after being cleared";
}

TEST_F(CommInterfaceImplTest, GetMeterData) {
    wavex_meter_data_t meter_data;

    comm->getMeterData(&meter_data);
    EXPECT_FALSE(meter_data.valid);

    stats->update_meter_data(0.5f, 0.3f, 0.8f, 0.6f);

    comm->getMeterData(&meter_data);
    EXPECT_TRUE(meter_data.valid);
    EXPECT_FLOAT_EQ(meter_data.rms_left, 0.5f);
    EXPECT_FLOAT_EQ(meter_data.rms_right, 0.3f);
    EXPECT_FLOAT_EQ(meter_data.peak_left, 0.8f);
    EXPECT_FLOAT_EQ(meter_data.peak_right, 0.6f);
}

TEST_F(CommInterfaceImplTest, SetBrowseResponseListener) {
    bool callback_called = false;
    comm->setBrowseResponseListener(comm_browse_callback, &callback_called);

    const uint8_t test_data[] = {0x01, 0x02, 0x03};
    stats->invoke_browse_resp_callback(test_data, sizeof(test_data));

    EXPECT_TRUE(callback_called);
}

TEST_F(CommInterfaceImplTest, SetStorageStatusListener) {
    static bool s_mounted;
    s_mounted = true;
    comm->setStorageStatusListener(
        [](bool mounted, void* user_data) {
            (void)user_data;
            s_mounted = mounted;
        },
        nullptr);

    stats->invoke_storage_status_callback(false);
    EXPECT_FALSE(s_mounted);
}

TEST_F(CommInterfaceImplTest, SetSampleStatusListener) {
    bool callback_called = false;
    comm->setSampleStatusListener(comm_sample_status_callback, &callback_called);

    stats->invoke_sample_status_callback(1, 1, 44100, 2, 1000);

    EXPECT_TRUE(callback_called);
}

// Sends must forward their arguments to the inter_mcu layer and return its
// result unchanged - the UI shows an error toast off this code.
TEST_F(CommInterfaceImplTest, SendBrowseRequestForwardsArgsAndResult) {
    EXPECT_EQ(comm->sendBrowseRequest("/samples", 20), ESP_OK);

    const auto& cap = GetInterMcuCapture();
    ASSERT_EQ(cap.browse_req_calls, 1);
    EXPECT_STREQ(cap.browse_req_path, "/samples");
    EXPECT_EQ(cap.browse_req_start_index, 20);
}

TEST_F(CommInterfaceImplTest, SendBrowseRequestPropagatesLinkFailure) {
    auto& cap = GetInterMcuCapture();
    cap.send_result = ESP_ERR_TIMEOUT;  // link down / TX queue full

    EXPECT_EQ(comm->sendBrowseRequest("/samples", 0), ESP_ERR_TIMEOUT);
    EXPECT_EQ(cap.browse_req_calls, 1);
}

TEST_F(CommInterfaceImplTest, SendSamplePlayRequestForwardsIndexAndResult) {
    EXPECT_EQ(comm->sendSamplePlayRequest(42), ESP_OK);

    const auto& cap = GetInterMcuCapture();
    ASSERT_EQ(cap.play_index_req_calls, 1);
    EXPECT_EQ(cap.play_index_file_index, 42u);

    GetInterMcuCapture().send_result = ESP_FAIL;
    EXPECT_EQ(comm->sendSamplePlayRequest(7), ESP_FAIL);
    EXPECT_EQ(cap.play_index_file_index, 7u);
}

TEST_F(CommInterfaceImplTest, SendSampleStopRequestForwardsResult) {
    EXPECT_EQ(comm->sendSampleStopRequest(), ESP_OK);
    EXPECT_EQ(GetInterMcuCapture().stop_req_calls, 1);

    GetInterMcuCapture().send_result = ESP_ERR_TIMEOUT;
    EXPECT_EQ(comm->sendSampleStopRequest(), ESP_ERR_TIMEOUT);
    EXPECT_EQ(GetInterMcuCapture().stop_req_calls, 2);
}

// sendSampleData validates its arguments BEFORE touching the link: null or
// empty buffers must be rejected without a send attempt.
TEST_F(CommInterfaceImplTest, SendSampleDataValidatesBeforeSending) {
    EXPECT_EQ(comm->sendSampleData(nullptr, 100), ESP_ERR_INVALID_ARG);
    uint8_t data[4] = {1, 2, 3, 4};
    EXPECT_EQ(comm->sendSampleData(data, 0), ESP_ERR_INVALID_ARG);
    EXPECT_EQ(GetInterMcuCapture().sample_data_calls, 0)
        << "invalid buffer reached the inter_mcu layer";

    EXPECT_EQ(comm->sendSampleData(data, sizeof(data)), ESP_OK);
    const auto& cap = GetInterMcuCapture();
    ASSERT_EQ(cap.sample_data_calls, 1);
    ASSERT_EQ(cap.sample_data.size(), sizeof(data));
    EXPECT_EQ(memcmp(cap.sample_data.data(), data, sizeof(data)), 0);
}

// Documented contract: the id-based load overload is not implemented in this
// class and must say so rather than silently succeed.
TEST_F(CommInterfaceImplTest, SendSampleLoadRequestIsRejectedByContract) {
    EXPECT_EQ(comm->sendSampleLoadRequest(1, 1024, 44100, 2, 16), ESP_ERR_INVALID_ARG);
}

TEST_F(CommInterfaceImplTest, GetBackendHeartbeat) {
    wavex_backend_heartbeat_t hb;

    comm->getBackendHeartbeat(&hb);
    EXPECT_FALSE(hb.valid);

    stats->update_backend_heartbeat(1000, 500, 100, 25.5f);

    comm->getBackendHeartbeat(&hb);
    EXPECT_TRUE(hb.valid);
    EXPECT_EQ(hb.uptime_ms, 1000u);
    EXPECT_EQ(hb.rx_total, 500u);
    EXPECT_EQ(hb.loop_counter, 100u);
    EXPECT_FLOAT_EQ(hb.cpu_usage_percent, 25.5f);
    // This getter flattens the detailed metrics to the legacy value.
    EXPECT_FLOAT_EQ(hb.cpu_avg_percent, 25.5f);
    EXPECT_FLOAT_EQ(hb.cpu_min_percent, 25.5f);
    EXPECT_FLOAT_EQ(hb.cpu_max_percent, 25.5f);
}

TEST_F(CommInterfaceImplTest, GetPacketStats) {
    wavex_packet_stats_t packet_stats;

    comm->getPacketStats(&packet_stats);
    EXPECT_EQ(packet_stats.total_packets, 0u);

    stats->increment_packet_stat(0x00);  // SYNC
    stats->increment_packet_stat(0x10);  // METER_PUSH

    comm->getPacketStats(&packet_stats);
    EXPECT_EQ(packet_stats.total_packets, 2u);
    EXPECT_EQ(packet_stats.sync_packets, 1u);
    EXPECT_EQ(packet_stats.meter_push_packets, 1u);
}

TEST_F(CommInterfaceImplTest, IsBusyReflectsInterMcuState) {
    EXPECT_FALSE(comm->isBusy());

    GetInterMcuCapture().busy = true;
    EXPECT_TRUE(comm->isBusy());
}

// Null out-pointers must be no-ops that leave the underlying state readable.
TEST_F(CommInterfaceImplTest, NullPointerHandling) {
    stats->update_meter_data(0.5f, 0.3f, 0.8f, 0.6f);

    comm->getMeterData(nullptr);
    comm->getBackendHeartbeat(nullptr);
    comm->getPacketStats(nullptr);

    wavex_meter_data_t meter_data;
    comm->getMeterData(&meter_data);
    EXPECT_TRUE(meter_data.valid);
    EXPECT_FLOAT_EQ(meter_data.rms_left, 0.5f);
}

}  // namespace
