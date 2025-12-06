#include "comm_interface_impl.h"

#include "../../shared/spi_protocol/protocol.h"
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

esp_err_t CommInterfaceImpl::sendSampleLoadRequest(uint16_t sample_id,
                                                   uint32_t sample_size,
                                                   uint16_t sample_rate,
                                                   uint8_t channels,
                                                   uint8_t bit_depth) {
    // Not used in this implementation; use the path-based overload directly via inter_mcu
    return ESP_ERR_INVALID_ARG;
}

esp_err_t CommInterfaceImpl::sendSampleData(const uint8_t* data, size_t length) {
    if (!data || length == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    return inter_mcu_send_sample_data(data, length);
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
