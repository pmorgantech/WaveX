#include "statistics.h"

#include <stdio.h>
#include <string.h>

#ifdef ESP_PLATFORM
#include "esp_log.h"
#include "esp_timer.h"
#ifdef WAVEX_TEST_BUILD
// Define FreeRTOS macros for test builds
#define pdMS_TO_TICKS(x) ((x) / 10)  // Assume 10ms tick period
#define pdTRUE 1
#define pdFALSE 0
#endif
#else
#include <stdio.h>
#define ESP_LOGI(tag, fmt, ...) printf("[%s] " fmt "\n", tag, ##__VA_ARGS__)
#define ESP_LOGW(tag, fmt, ...) printf("[%s] WARN: " fmt "\n", tag, ##__VA_ARGS__)
// Define FreeRTOS macros for non-ESP_PLATFORM builds
#define pdMS_TO_TICKS(x) ((x) / 10)  // Assume 10ms tick period
#define pdTRUE 1
#define pdFALSE 0
#endif

StatisticsManager::StatisticsManager() {
    memset(&m_packet_stats, 0, sizeof(m_packet_stats));
    memset(&m_tx_stats, 0, sizeof(m_tx_stats));
    memset(&m_backend_hb, 0, sizeof(m_backend_hb));
    memset(&m_meter_data, 0, sizeof(m_meter_data));

#ifdef ESP_PLATFORM
    ESP_LOGD("StatisticsManager", "=== Initializing locks for ESP_PLATFORM ===");
    m_stats_lock = portMUX_INITIALIZER_UNLOCKED;
    m_tx_stats_lock = portMUX_INITIALIZER_UNLOCKED;
    m_hb_lock = portMUX_INITIALIZER_UNLOCKED;
    m_meter_lock = portMUX_INITIALIZER_UNLOCKED;
    ESP_LOGD("StatisticsManager", "=== Locks initialized successfully ===");
#else
    ESP_LOGD("StatisticsManager", "=== Initializing locks for non-ESP_PLATFORM ===");
    memset(&m_stats_lock, 0, sizeof(m_stats_lock));
    memset(&m_tx_stats_lock, 0, sizeof(m_tx_stats_lock));
    memset(&m_hb_lock, 0, sizeof(m_hb_lock));
    memset(&m_meter_lock, 0, sizeof(m_meter_lock));
    ESP_LOGD("StatisticsManager", "=== Locks initialized successfully ===");
#endif
}

void StatisticsManager::increment_packet_stat(uint8_t packet_type) {
    taskENTER_CRITICAL(&m_stats_lock);
    m_packet_stats.total_packets++;

    switch (packet_type) {
        case 0x00:
            m_packet_stats.sync_packets++;
            break;  // MSG_SYNC
        case 0x01:
            m_packet_stats.control_change_packets++;
            break;  // MSG_CONTROL_CHANGE
        case 0x02:
            m_packet_stats.note_on_packets++;
            break;  // MSG_NOTE_ON
        case 0x03:
            m_packet_stats.note_off_packets++;
            break;  // MSG_NOTE_OFF
        case 0x04:
            m_packet_stats.sample_load_packets++;
            break;  // MSG_SAMPLE_LOAD
        case 0x05:
            m_packet_stats.sample_data_packets++;
            break;  // MSG_SAMPLE_DATA
        case 0x06:
            m_packet_stats.parameter_update_packets++;
            break;  // MSG_PARAMETER_UPDATE
        case 0x07:
            m_packet_stats.status_request_packets++;
            break;  // MSG_STATUS_REQUEST
        case 0x08:
            m_packet_stats.status_response_packets++;
            break;  // MSG_STATUS_RESPONSE
        case 0x09:
            m_packet_stats.sample_ctrl_packets++;
            break;  // MSG_SAMPLE_CTRL
        case 0x0A:
            m_packet_stats.preview_req_packets++;
            break;  // MSG_PREVIEW_REQ
        case 0x0B:  // MSG_DATA_REQUEST
            m_packet_stats.data_request_packets++;
            break;
        case 0x0D:  // Legacy MSG_METER_PUSH
        case 0x10:
            m_packet_stats.meter_push_packets++;
            break;  // Current MSG_METER_PUSH (0x10)
        case 0x0E:  // Legacy MSG_WAVE_CHUNK
        case 0x11:
            m_packet_stats.wave_chunk_packets++;
            break;  // Current MSG_WAVE_CHUNK (0x11)
        case 0x0F:  // Legacy MSG_HEARTBEAT
        case 0x12:
            m_packet_stats.heartbeat_packets++;
            break;  // Current MSG_HEARTBEAT (0x12)
        case 0x3B:
            m_packet_stats.diag_push_packets++;
            break;  // MSG_DIAG_PUSH
        case 0x44:
            m_packet_stats.envelope_chunk_packets++;
            break;  // MSG_ENVELOPE_CHUNK
        case 0xFF:
            m_packet_stats.error_packets++;
            break;  // MSG_ERROR
        // Recognised, but with no counter of their own. These are the busiest
        // messages on the live link, and counting them as "unknown" - which is
        // what happened before - drowned the signal that counter exists for.
        case 0x31:  // MSG_BROWSE_RESP
        case 0x34:  // MSG_SAMPLE_STATUS
        case 0x39:  // MSG_STORAGE_STATUS
        case 0x3D:  // MSG_SAMPLE_META
        case 0x42:  // MSG_CV_CAL_RESP
        case 0x61:  // MSG_INST_STATUS
            m_packet_stats.other_known_packets++;
            break;
        default:
            m_packet_stats.unknown_packets++;
            break;
    }
    taskEXIT_CRITICAL(&m_stats_lock);
}

void StatisticsManager::increment_invalid_packet() {
    taskENTER_CRITICAL(&m_stats_lock);
    m_packet_stats.total_packets++;
    m_packet_stats.invalid_packets++;
    taskEXIT_CRITICAL(&m_stats_lock);
}

void StatisticsManager::get_packet_stats(wavex_packet_stats_t* out) const {
    if (!out)
        return;
    taskENTER_CRITICAL(&m_stats_lock);
    *out = m_packet_stats;
    taskEXIT_CRITICAL(&m_stats_lock);
}

void StatisticsManager::reset_packet_stats() {
    taskENTER_CRITICAL(&m_stats_lock);
    memset(&m_packet_stats, 0, sizeof(m_packet_stats));
    taskEXIT_CRITICAL(&m_stats_lock);
}

void StatisticsManager::get_packet_summary(wavex_packet_summary_t* out) const {
    if (!out)
        return;
    taskENTER_CRITICAL(&m_stats_lock);
    out->total_packets = m_packet_stats.total_packets;
    out->meter_packets = m_packet_stats.meter_push_packets;
    out->heartbeat_packets = m_packet_stats.heartbeat_packets;
    out->control_packets = m_packet_stats.control_change_packets + m_packet_stats.note_on_packets +
                           m_packet_stats.note_off_packets + m_packet_stats.sample_ctrl_packets;
    out->invalid_packets = m_packet_stats.invalid_packets;
    taskEXIT_CRITICAL(&m_stats_lock);
}

uint32_t StatisticsManager::get_meter_packet_count() const {
    uint32_t count;
    taskENTER_CRITICAL(&m_stats_lock);
    count = m_packet_stats.meter_push_packets;
    taskEXIT_CRITICAL(&m_stats_lock);
    return count;
}

uint32_t StatisticsManager::get_total_packet_count() const {
    uint32_t count;
    taskENTER_CRITICAL(&m_stats_lock);
    count = m_packet_stats.total_packets;
    taskEXIT_CRITICAL(&m_stats_lock);
    return count;
}

int StatisticsManager::format_packet_stats(char* buffer, size_t buffer_size) const {
    if (!buffer || buffer_size == 0)
        return 0;

    taskENTER_CRITICAL(&m_stats_lock);
    wavex_packet_stats_t stats = m_packet_stats;
    taskEXIT_CRITICAL(&m_stats_lock);

    return snprintf(buffer,
                    buffer_size,
                    "Packets: Total=%lu, Valid=%lu, Invalid=%lu | "
                    "METER=%lu, HEARTBEAT=%lu, SYNC=%lu, WAVE=%lu, CTRL=%lu",
                    (unsigned long)stats.total_packets,
                    (unsigned long)(stats.total_packets - stats.invalid_packets),
                    (unsigned long)stats.invalid_packets,
                    (unsigned long)stats.meter_push_packets,
                    (unsigned long)stats.heartbeat_packets,
                    (unsigned long)stats.sync_packets,
                    (unsigned long)stats.wave_chunk_packets,
                    (unsigned long)(stats.control_change_packets + stats.note_on_packets +
                                    stats.note_off_packets + stats.sample_ctrl_packets));
}

void StatisticsManager::increment_tx_message(uint8_t message_type) {
    taskENTER_CRITICAL(&m_tx_stats_lock);
    m_tx_stats.total_messages_sent++;

    switch (message_type) {
        case 0x01:
            m_tx_stats.ping_messages_sent++;
            break;
        case 0x02:
            m_tx_stats.test_messages_sent++;
            break;
        default:
            break;
    }

#ifdef ESP_PLATFORM
    m_tx_stats.last_send_time = (uint32_t)(esp_timer_get_time() / 1000);
#else
    m_tx_stats.last_send_time = 0;
#endif
    taskEXIT_CRITICAL(&m_tx_stats_lock);
}

void StatisticsManager::get_tx_stats(wavex_tx_stats_t* out) const {
    if (!out)
        return;
    taskENTER_CRITICAL(&m_tx_stats_lock);
    *out = m_tx_stats;
    taskEXIT_CRITICAL(&m_tx_stats_lock);
}

void StatisticsManager::update_backend_heartbeat(uint32_t uptime_ms,
                                                 uint32_t rx_total,
                                                 uint32_t loop_counter,
                                                 float cpu_usage_percent) {
    taskENTER_CRITICAL(&m_hb_lock);
    m_backend_hb.uptime_ms = uptime_ms;
    m_backend_hb.rx_total = rx_total;
    m_backend_hb.loop_counter = loop_counter;
    m_backend_hb.cpu_usage_percent = cpu_usage_percent;
    // For backward compatibility, set all CPU metrics to the same value
    m_backend_hb.cpu_avg_percent = cpu_usage_percent;
    m_backend_hb.cpu_min_percent = cpu_usage_percent;
    m_backend_hb.cpu_max_percent = cpu_usage_percent;
#ifdef ESP_PLATFORM
    m_backend_hb.last_rx_ms = (uint32_t)(esp_timer_get_time() / 1000);
#else
    m_backend_hb.last_rx_ms = 0;
#endif
    m_backend_hb.valid = true;
    taskEXIT_CRITICAL(&m_hb_lock);
}

void StatisticsManager::update_backend_heartbeat_detailed(uint32_t uptime_ms,
                                                          uint32_t rx_total,
                                                          uint32_t loop_counter,
                                                          float cpu_avg_percent,
                                                          float cpu_min_percent,
                                                          float cpu_max_percent) {
    taskENTER_CRITICAL(&m_hb_lock);
    m_backend_hb.uptime_ms = uptime_ms;
    m_backend_hb.rx_total = rx_total;
    m_backend_hb.loop_counter = loop_counter;
    m_backend_hb.cpu_usage_percent = cpu_avg_percent;  // Legacy compatibility
    m_backend_hb.cpu_avg_percent = cpu_avg_percent;
    m_backend_hb.cpu_min_percent = cpu_min_percent;
    m_backend_hb.cpu_max_percent = cpu_max_percent;
#ifdef ESP_PLATFORM
    m_backend_hb.last_rx_ms = (uint32_t)(esp_timer_get_time() / 1000);
#else
    m_backend_hb.last_rx_ms = 0;
#endif
    m_backend_hb.valid = true;
    taskEXIT_CRITICAL(&m_hb_lock);
}

void StatisticsManager::get_backend_heartbeat(uint32_t* uptime_ms,
                                              uint32_t* rx_total,
                                              uint32_t* loop_counter,
                                              uint32_t* last_rx_ms,
                                              float* cpu_usage_percent,
                                              bool* valid) const {
    if (!uptime_ms || !rx_total || !loop_counter || !last_rx_ms || !cpu_usage_percent || !valid)
        return;

    taskENTER_CRITICAL(&m_hb_lock);
    *uptime_ms = m_backend_hb.uptime_ms;
    *rx_total = m_backend_hb.rx_total;
    *loop_counter = m_backend_hb.loop_counter;
    *last_rx_ms = m_backend_hb.last_rx_ms;
    *cpu_usage_percent = m_backend_hb.cpu_usage_percent;
    *valid = m_backend_hb.valid;
    taskEXIT_CRITICAL(&m_hb_lock);
}

void StatisticsManager::get_backend_heartbeat_detailed(uint32_t* uptime_ms,
                                                       uint32_t* rx_total,
                                                       uint32_t* loop_counter,
                                                       uint32_t* last_rx_ms,
                                                       float* cpu_avg_percent,
                                                       float* cpu_min_percent,
                                                       float* cpu_max_percent,
                                                       bool* valid) const {
    if (!uptime_ms || !rx_total || !loop_counter || !last_rx_ms || !cpu_avg_percent ||
        !cpu_min_percent || !cpu_max_percent || !valid)
        return;

    taskENTER_CRITICAL(&m_hb_lock);
    *uptime_ms = m_backend_hb.uptime_ms;
    *rx_total = m_backend_hb.rx_total;
    *loop_counter = m_backend_hb.loop_counter;
    *last_rx_ms = m_backend_hb.last_rx_ms;
    *cpu_avg_percent = m_backend_hb.cpu_avg_percent;
    *cpu_min_percent = m_backend_hb.cpu_min_percent;
    *cpu_max_percent = m_backend_hb.cpu_max_percent;
    *valid = m_backend_hb.valid;
    taskEXIT_CRITICAL(&m_hb_lock);
}

void StatisticsManager::update_tx_stats(uint8_t message_type) {
    increment_tx_message(message_type);
}

void StatisticsManager::update_meter_data(float rms_left,
                                          float rms_right,
                                          float peak_left,
                                          float peak_right) {
    taskENTER_CRITICAL(&m_meter_lock);
    m_meter_data.rms_left = rms_left;
    m_meter_data.rms_right = rms_right;
    m_meter_data.peak_left = peak_left;
    m_meter_data.peak_right = peak_right;
#ifdef ESP_PLATFORM
    m_meter_data.last_update_ms = (uint32_t)(esp_timer_get_time() / 1000);
#else
    m_meter_data.last_update_ms = 0;
#endif
    m_meter_data.valid = true;
    taskEXIT_CRITICAL(&m_meter_lock);

    // Outside the spinlock above: that one guards the meter snapshot, and a
    // spinlock must not span a callback. The slot has its own mutex.
    m_meter_listener.invoke(rms_left, rms_right, peak_left, peak_right);
}

void StatisticsManager::get_meter_data(wavex_meter_data_t* out) const {
    if (!out)
        return;

    taskENTER_CRITICAL(&m_meter_lock);
    *out = m_meter_data;
    taskEXIT_CRITICAL(&m_meter_lock);
}

void StatisticsManager::set_meter_callback(void (*callback)(float rms_left,
                                                            float rms_right,
                                                            float peak_left,
                                                            float peak_right,
                                                            void* user_data),
                                           void* user_data) {
    m_meter_listener.set(callback, user_data);
}

void StatisticsManager::set_storage_status_callback(void (*callback)(bool mounted, void* user_data),
                                                    void* user_data) {
    m_storage_status_listener.set(callback, user_data);
}

void StatisticsManager::invoke_storage_status_callback(bool mounted) {
    m_storage_status_listener.invoke(mounted);
}

void StatisticsManager::set_browse_resp_callback(
    void (*callback)(const uint8_t* data, size_t length, void* user_data), void* user_data) {
    m_browse_resp_listener.set(callback, user_data);
    ESP_LOGD("StatisticsManager", "Browse response callback registered: %p", callback);
}

void StatisticsManager::invoke_browse_resp_callback(const uint8_t* data, size_t length) {
    m_browse_resp_listener.invoke(data, length);
}

void StatisticsManager::set_sample_status_callback(void (*callback)(uint16_t sample_id,
                                                                    uint8_t state,
                                                                    uint32_t sample_rate,
                                                                    uint8_t channels,
                                                                    uint32_t frames_played,
                                                                    void* user_data),
                                                   void* user_data) {
    m_sample_status_listener.set(callback, user_data);
}

void StatisticsManager::invoke_sample_status_callback(uint16_t sample_id,
                                                      uint8_t state,
                                                      uint32_t sample_rate,
                                                      uint8_t channels,
                                                      uint32_t frames_played) {
    // This used to release the mutex before calling, commented "to avoid
    // deadlocks". That gave up the only thing the mutex was buying - a page
    // deregistering in onExit could return while its handler was still running
    // and then be destroyed under it - and read m_sample_status_user_data after
    // releasing, so the pair could tear as well.
    m_sample_status_listener.invoke(sample_id, state, sample_rate, channels, frames_played);
}
