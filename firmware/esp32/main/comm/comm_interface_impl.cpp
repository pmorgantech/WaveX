#include "comm_interface_impl.h"

#include "inter_mcu.h"
#include "spi_protocol/protocol.h"

namespace WaveX {
namespace Comm {

CommInterfaceImpl::CommInterfaceImpl(StatisticsManager& statistics) : statistics_(statistics) {}

// File browsing operations
void CommInterfaceImpl::setBrowseResponseListener(wavex_browse_resp_cb_t cb, void* user_data) {
    statistics_.set_browse_resp_callback(cb, user_data);
}

void CommInterfaceImpl::setStorageStatusListener(wavex_storage_status_cb_t cb, void* user_data) {
    statistics_.set_storage_status_callback(cb, user_data);
}

esp_err_t CommInterfaceImpl::sendBrowsePageRequest(const Protocol::BrowsePageRequest& request) {
    return inter_mcu_send_browse_page_req(request);
}

// Sample control operations
void CommInterfaceImpl::setSampleStatusListener(wavex_sample_status_cb_t cb, void* user_data) {
    statistics_.set_sample_status_callback(cb, user_data);
}

esp_err_t CommInterfaceImpl::sendSamplePlayRequest(uint32_t file_index) {
    return inter_mcu_send_sample_play_index_req(file_index);
}

esp_err_t CommInterfaceImpl::sendSampleStopRequest() {
    return inter_mcu_send_sample_stop_req();
}

}  // namespace Comm
}  // namespace WaveX
