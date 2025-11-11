#include "comm_interface_impl.h"

#include "../inter_mcu.h"

namespace WaveX {
namespace Comm {

// Constructor with dependency injection
CommInterfaceImpl::CommInterfaceImpl(StatisticsManager& statistics) : statistics_(statistics) {}

// Meter data operations
void CommInterfaceImpl::setMeterListener(wavex_meter_cb_t cb, void* user_data) {
    statistics_.set_meter_callback(cb, user_data);
}

void CommInterfaceImpl::getMeterData(wavex_meter_data_t* out) {
    statistics_.get_meter_data(out);
}

// File browsing operations
void CommInterfaceImpl::setBrowseResponseListener(wavex_browse_resp_cb_t cb, void* user_data) {
    statistics_.set_browse_resp_callback(cb, user_data);
}

esp_err_t CommInterfaceImpl::sendBrowseRequest(const char* path, uint8_t start_index) {
    return inter_mcu_send_browse_req(path, start_index);
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

// Diagnostics operations
void CommInterfaceImpl::getBackendHeartbeat(wavex_backend_heartbeat_t* out) {
    if (!out)
        return;

    uint32_t uptime_ms, rx_total, loop_counter, last_rx_ms;
    float cpu_usage_percent;
    bool valid;

    statistics_.get_backend_heartbeat(
        &uptime_ms, &rx_total, &loop_counter, &last_rx_ms, &cpu_usage_percent, &valid);

    // Fill the output struct
    out->uptime_ms = uptime_ms;
    out->rx_total = rx_total;
    out->loop_counter = loop_counter;
    out->last_rx_ms = last_rx_ms;
    out->cpu_usage_percent = cpu_usage_percent;
    out->cpu_avg_percent = cpu_usage_percent;  // For compatibility
    out->cpu_min_percent = cpu_usage_percent;
    out->cpu_max_percent = cpu_usage_percent;
    out->valid = valid;
}

void CommInterfaceImpl::getPacketStats(wavex_packet_stats_t* out) {
    statistics_.get_packet_stats(out);
}

// Utility operations
bool CommInterfaceImpl::isBusy() {
    return inter_mcu_is_busy();
}

}  // namespace Comm
}  // namespace WaveX
