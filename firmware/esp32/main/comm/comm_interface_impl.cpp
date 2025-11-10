#include "comm_interface_impl.h"

#include "../inter_mcu.h"

namespace WaveX {
namespace Comm {

namespace {
CommInterfaceImpl g_comm_interface_instance;
}

// Meter data operations
void CommInterfaceImpl::setMeterListener(wavex_meter_cb_t cb, void* user_data) {
    inter_mcu_set_meter_listener(cb, user_data);
}

void CommInterfaceImpl::getMeterData(wavex_meter_data_t* out) {
    inter_mcu_get_meter_data(out);
}

// File browsing operations
void CommInterfaceImpl::setBrowseResponseListener(wavex_browse_resp_cb_t cb, void* user_data) {
    inter_mcu_set_browse_resp_listener(cb, user_data);
}

esp_err_t CommInterfaceImpl::sendBrowseRequest(const char* path, uint8_t start_index) {
    return inter_mcu_send_browse_req(path, start_index);
}

// Sample control operations
void CommInterfaceImpl::setSampleStatusListener(wavex_sample_status_cb_t cb, void* user_data) {
    inter_mcu_set_sample_status_listener(cb, user_data);
}

esp_err_t CommInterfaceImpl::sendSamplePlayRequest(uint32_t file_index) {
    return inter_mcu_send_sample_play_index_req(file_index);
}

esp_err_t CommInterfaceImpl::sendSampleStopRequest() {
    return inter_mcu_send_sample_stop_req();
}

// Diagnostics operations
void CommInterfaceImpl::getBackendHeartbeat(wavex_backend_heartbeat_t* out) {
    inter_mcu_get_backend_heartbeat_detailed(out);
}

void CommInterfaceImpl::getPacketStats(wavex_packet_stats_t* out) {
    inter_mcu_get_packet_stats(out);
}

// Utility operations
bool CommInterfaceImpl::isBusy() {
    return inter_mcu_is_busy();
}

// Factory function implementation
ICommInterface* GetCommInterface() {
    return &g_comm_interface_instance;
}

}  // namespace Comm
}  // namespace WaveX
