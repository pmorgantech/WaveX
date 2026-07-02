/**
 * @file mock_application_context.h
 * @brief Mock ApplicationContext for testing
 */

#pragma once

#include <gmock/gmock.h>

#include "application_context.h"
#include "comm/i_comm_interface.h"
#include "comm/packet_router.h"
#include "comm/statistics.h"

namespace WaveX {

// Mock StatisticsManager
class MockStatisticsManager : public StatisticsManager {
   public:
    MockStatisticsManager() = default;
    ~MockStatisticsManager() override = default;

    MOCK_METHOD(void, set_meter_callback, (wavex_meter_cb_t cb, void* user_data), (override));
    MOCK_METHOD(void, get_meter_data, (wavex_meter_data_t * out), (override));
    MOCK_METHOD(void,
                set_browse_resp_callback,
                (wavex_browse_resp_cb_t cb, void* user_data),
                (override));
    MOCK_METHOD(void,
                set_sample_status_callback,
                (wavex_sample_status_cb_t cb, void* user_data),
                (override));
    MOCK_METHOD(void, get_backend_heartbeat, (wavex_backend_heartbeat_t * out), (override));
    MOCK_METHOD(void, get_packet_stats, (wavex_packet_stats_t * out), (override));
    MOCK_METHOD(void, reset_packet_stats, (), (override));
    MOCK_METHOD(void, increment_packet_count, (uint8_t packet_type), (override));
    MOCK_METHOD(void, update_last_packet_time, (), (override));
    MOCK_METHOD(void, update_backend_heartbeat, (uint8_t state), (override));
    MOCK_METHOD(void,
                update_sample_status,
                (uint16_t sample_id,
                 uint8_t state,
                 uint32_t sample_rate,
                 uint8_t channels,
                 uint32_t frames_played),
                (override));
};

// Mock PacketRouter
class MockPacketRouter : public Comm::PacketRouter {
   public:
    MockPacketRouter() = default;
    ~MockPacketRouter() override = default;

    MOCK_METHOD(void, route_packet, (const uint8_t* data, size_t length), (override));
    MOCK_METHOD(void, set_stats_callback, (packet_stats_callback_t callback), (override));
    MOCK_METHOD(void, handle_sync, (), (override));
};

// Mock ICommInterface
class MockICommInterface : public Comm::ICommInterface {
   public:
    MockICommInterface() = default;
    ~MockICommInterface() override = default;

    MOCK_METHOD(void, setMeterListener, (wavex_meter_cb_t cb, void* user_data), (override));
    MOCK_METHOD(void, getMeterData, (wavex_meter_data_t * out), (override));
    MOCK_METHOD(void,
                setBrowseResponseListener,
                (wavex_browse_resp_cb_t cb, void* user_data),
                (override));
    MOCK_METHOD(esp_err_t, sendBrowseRequest, (const char* path, uint8_t start_index), (override));
    MOCK_METHOD(void,
                setSampleStatusListener,
                (wavex_sample_status_cb_t cb, void* user_data),
                (override));
    MOCK_METHOD(esp_err_t, sendSamplePlayRequest, (uint32_t file_index), (override));
    MOCK_METHOD(esp_err_t, sendSampleStopRequest, (), (override));
    MOCK_METHOD(void, getBackendHeartbeat, (wavex_backend_heartbeat_t * out), (override));
    MOCK_METHOD(void, getPacketStats, (wavex_packet_stats_t * out), (override));
    MOCK_METHOD(bool, isBusy, (), (override));
};

// Mock ApplicationContext
class MockApplicationContext {
   public:
    MockApplicationContext() {
        // Set up default behaviors for mocks
        ON_CALL(*statistics_, get_meter_data(testing::_))
            .WillByDefault(testing::Invoke([](wavex_meter_data_t* out) {
                if (out) {
                    memset(out, 0, sizeof(wavex_meter_data_t));
                }
            }));

        ON_CALL(*statistics_, get_backend_heartbeat(testing::_))
            .WillByDefault(testing::Invoke([](wavex_backend_heartbeat_t* out) {
                if (out) {
                    memset(out, 0, sizeof(wavex_backend_heartbeat_t));
                }
            }));

        ON_CALL(*statistics_, get_packet_stats(testing::_))
            .WillByDefault(testing::Invoke([](wavex_packet_stats_t* out) {
                if (out) {
                    memset(out, 0, sizeof(wavex_packet_stats_t));
                }
            }));

        ON_CALL(*comm_interface_, isBusy()).WillByDefault(testing::Return(false));

        ON_CALL(*comm_interface_, sendBrowseRequest(testing::_, testing::_))
            .WillByDefault(testing::Return(ESP_OK));

        ON_CALL(*comm_interface_, sendSamplePlayRequest(testing::_))
            .WillByDefault(testing::Return(ESP_OK));

        ON_CALL(*comm_interface_, sendSampleStopRequest()).WillByDefault(testing::Return(ESP_OK));
    }

    // Mock components
    std::unique_ptr<MockStatisticsManager> statistics_ = std::make_unique<MockStatisticsManager>();
    std::unique_ptr<MockPacketRouter> packet_router_ = std::make_unique<MockPacketRouter>();
    std::unique_ptr<MockICommInterface> comm_interface_ = std::make_unique<MockICommInterface>();

    // Accessors matching ApplicationContext interface
    MockStatisticsManager& getStatistics() { return *statistics_; }
    MockPacketRouter& getPacketRouter() { return *packet_router_; }
    MockICommInterface& getCommInterface() { return *comm_interface_; }

    const MockStatisticsManager& getStatistics() const { return *statistics_; }
    const MockPacketRouter& getPacketRouter() const { return *packet_router_; }
    const MockICommInterface& getCommInterface() const { return *comm_interface_; }
};

}  // namespace WaveX
