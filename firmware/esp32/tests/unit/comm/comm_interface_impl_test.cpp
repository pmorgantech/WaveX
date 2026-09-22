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
TEST_F(CommInterfaceImplTest, SendBrowsePageRequestForwardsArgsAndResult) {
    WaveX::Protocol::BrowsePageRequest request;
    request.request_id = 123;
    request.start_index = 20;
    request.filter = WaveX::Protocol::BrowseFilter::Instruments;
    strcpy(request.path, "/samples");
    EXPECT_EQ(comm->sendBrowsePageRequest(request), ESP_OK);

    const auto& cap = GetInterMcuCapture();
    ASSERT_EQ(cap.browse_req_calls, 1);
    EXPECT_STREQ(cap.browse_req_path, "/samples");
    EXPECT_EQ(cap.browse_req_start_index, 20);
    EXPECT_EQ(cap.browse_request_id, 123u);
    EXPECT_EQ(cap.browse_req_filter, WaveX::Protocol::BrowseFilter::Instruments);
}

TEST_F(CommInterfaceImplTest, SendBrowsePageRequestPropagatesLinkFailure) {
    WaveX::Protocol::BrowsePageRequest request;
    request.request_id = 124;
    strcpy(request.path, "/samples");
    auto& cap = GetInterMcuCapture();
    cap.send_result = ESP_ERR_TIMEOUT;  // link down / TX queue full

    EXPECT_EQ(comm->sendBrowsePageRequest(request), ESP_ERR_TIMEOUT);
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

}  // namespace
