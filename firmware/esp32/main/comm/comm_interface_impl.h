#pragma once

#include "comm/i_comm_interface.h"
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

    // File browsing operations
    void setBrowseResponseListener(wavex_browse_resp_cb_t cb, void* user_data) override;
    void setStorageStatusListener(wavex_storage_status_cb_t cb, void* user_data) override;

    esp_err_t sendBrowsePageRequest(const Protocol::BrowsePageRequest& request) override;

    // Sample control operations
    void setSampleStatusListener(wavex_sample_status_cb_t cb, void* user_data) override;
    esp_err_t sendSamplePlayRequest(uint32_t file_index) override;
    esp_err_t sendSampleStopRequest() override;

   private:
    StatisticsManager& statistics_;
};

}  // namespace Comm
}  // namespace WaveX
