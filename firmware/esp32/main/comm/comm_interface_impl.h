#pragma once

#include "i_comm_interface.h"
#include "statistics.h"

namespace WaveX {
namespace Comm {

/**
 * @brief Concrete implementation of ICommInterface using injected StatisticsManager
 *
 * This implementation uses dependency injection to access StatisticsManager
 * instead of global state, providing a clean interface for the UI layer.
 */
class CommInterfaceImpl : public ICommInterface {
   public:
    /**
     * @brief Construct with injected StatisticsManager dependency
     * @param statistics Reference to StatisticsManager (owned by ApplicationContext)
     */
    explicit CommInterfaceImpl(StatisticsManager& statistics);

    ~CommInterfaceImpl() override = default;

    // Meter data operations
    void setMeterListener(wavex_meter_cb_t cb, void* user_data) override;
    void getMeterData(wavex_meter_data_t* out) override;

    // File browsing operations
    void setBrowseResponseListener(wavex_browse_resp_cb_t cb, void* user_data) override;
    esp_err_t sendBrowseRequest(const char* path, uint8_t start_index) override;

    // Sample control operations
    void setSampleStatusListener(wavex_sample_status_cb_t cb, void* user_data) override;
    esp_err_t sendSamplePlayRequest(uint32_t file_index) override;
    esp_err_t sendSampleStopRequest() override;

    // Diagnostics operations
    void getBackendHeartbeat(wavex_backend_heartbeat_t* out) override;
    void getPacketStats(wavex_packet_stats_t* out) override;

    // Utility operations
    bool isBusy() override;

   private:
    StatisticsManager& statistics_;  // Injected dependency reference
};

}  // namespace Comm
}  // namespace WaveX
