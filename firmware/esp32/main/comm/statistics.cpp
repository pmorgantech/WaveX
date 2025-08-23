#include "statistics.h"
#include <string.h>
#include <stdio.h>

#ifdef ESP_PLATFORM
#include "esp_timer.h"
#endif

StatisticsManager::StatisticsManager()
{
    memset(&m_packet_stats, 0, sizeof(m_packet_stats));
    memset(&m_tx_stats, 0, sizeof(m_tx_stats));
    memset(&m_backend_hb, 0, sizeof(m_backend_hb));
    
#ifdef ESP_PLATFORM
    m_stats_lock = portMUX_INITIALIZER_UNLOCKED;
    m_tx_stats_lock = portMUX_INITIALIZER_UNLOCKED;
    m_hb_lock = portMUX_INITIALIZER_UNLOCKED;
#else
    memset(&m_stats_lock, 0, sizeof(m_stats_lock));
    memset(&m_tx_stats_lock, 0, sizeof(m_tx_stats_lock));
    memset(&m_hb_lock, 0, sizeof(m_hb_lock));
#endif
}

void StatisticsManager::increment_packet_stat(uint8_t packet_type)
{
    taskENTER_CRITICAL(&m_stats_lock);
    m_packet_stats.total_packets++;
    
    switch (packet_type) {
        case 0x01: m_packet_stats.sync_packets++; break;
        case 0x02: m_packet_stats.control_change_packets++; break;
        case 0x03: m_packet_stats.note_on_packets++; break;
        case 0x04: m_packet_stats.note_off_packets++; break;
        case 0x05: m_packet_stats.sample_load_packets++; break;
        case 0x06: m_packet_stats.sample_data_packets++; break;
        case 0x07: m_packet_stats.parameter_update_packets++; break;
        case 0x08: m_packet_stats.status_request_packets++; break;
        case 0x09: m_packet_stats.status_response_packets++; break;
        case 0x0A: m_packet_stats.sample_ctrl_packets++; break;
        case 0x0B: m_packet_stats.preview_req_packets++; break;
        case 0x0C: m_packet_stats.data_request_packets++; break;
        case 0x0D: m_packet_stats.meter_push_packets++; break;
        case 0x0E: m_packet_stats.wave_chunk_packets++; break;
        case 0x0F: m_packet_stats.heartbeat_packets++; break;
        case 0xFF: m_packet_stats.error_packets++; break;
        default: m_packet_stats.unknown_packets++; break;
    }
    taskEXIT_CRITICAL(&m_stats_lock);
}

void StatisticsManager::increment_invalid_packet()
{
    taskENTER_CRITICAL(&m_stats_lock);
    m_packet_stats.invalid_packets++;
    taskEXIT_CRITICAL(&m_stats_lock);
}

void StatisticsManager::get_packet_stats(wavex_packet_stats_t* out) const
{
    if (!out) return;
    taskENTER_CRITICAL(&m_stats_lock);
    *out = m_packet_stats;
    taskEXIT_CRITICAL(&m_stats_lock);
}

void StatisticsManager::reset_packet_stats()
{
    taskENTER_CRITICAL(&m_stats_lock);
    memset(&m_packet_stats, 0, sizeof(m_packet_stats));
    taskEXIT_CRITICAL(&m_stats_lock);
}

void StatisticsManager::get_packet_summary(wavex_packet_summary_t* out) const
{
    if (!out) return;
    taskENTER_CRITICAL(&m_stats_lock);
    out->total_packets = m_packet_stats.total_packets;
    out->meter_packets = m_packet_stats.meter_push_packets;
    out->heartbeat_packets = m_packet_stats.heartbeat_packets;
    out->control_packets = m_packet_stats.control_change_packets + 
                           m_packet_stats.note_on_packets + 
                           m_packet_stats.note_off_packets + 
                           m_packet_stats.sample_ctrl_packets;
    out->invalid_packets = m_packet_stats.invalid_packets;
    taskEXIT_CRITICAL(&m_stats_lock);
}

uint32_t StatisticsManager::get_meter_packet_count() const
{
    uint32_t count;
    taskENTER_CRITICAL(&m_stats_lock);
    count = m_packet_stats.meter_push_packets;
    taskEXIT_CRITICAL(&m_stats_lock);
    return count;
}

uint32_t StatisticsManager::get_total_packet_count() const
{
    uint32_t count;
    taskENTER_CRITICAL(&m_stats_lock);
    count = m_packet_stats.total_packets;
    taskEXIT_CRITICAL(&m_stats_lock);
    return count;
}

int StatisticsManager::format_packet_stats(char* buffer, size_t buffer_size) const
{
    if (!buffer || buffer_size == 0) return 0;
    
    taskENTER_CRITICAL(&m_stats_lock);
    wavex_packet_stats_t stats = m_packet_stats;
    taskEXIT_CRITICAL(&m_stats_lock);
    
    return snprintf(buffer, buffer_size,
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

void StatisticsManager::increment_tx_message(uint8_t message_type)
{
    taskENTER_CRITICAL(&m_tx_stats_lock);
    m_tx_stats.total_messages_sent++;
    
    switch (message_type) {
        case 0x01: m_tx_stats.ping_messages_sent++; break;
        case 0x02: m_tx_stats.test_messages_sent++; break;
        default: break;
    }
    
#ifdef ESP_PLATFORM
    m_tx_stats.last_send_time = (uint32_t)(esp_timer_get_time() / 1000);
#else
    m_tx_stats.last_send_time = 0;
#endif
    taskEXIT_CRITICAL(&m_tx_stats_lock);
}

void StatisticsManager::get_tx_stats(wavex_tx_stats_t* out) const
{
    if (!out) return;
    taskENTER_CRITICAL(&m_tx_stats_lock);
    *out = m_tx_stats;
    taskEXIT_CRITICAL(&m_tx_stats_lock);
}

void StatisticsManager::update_backend_heartbeat(uint32_t uptime_ms, uint32_t rx_total, uint32_t loop_counter)
{
    taskENTER_CRITICAL(&m_hb_lock);
    m_backend_hb.uptime_ms = uptime_ms;
    m_backend_hb.rx_total = rx_total;
    m_backend_hb.loop_counter = loop_counter;
#ifdef ESP_PLATFORM
    m_backend_hb.last_rx_ms = (uint32_t)(esp_timer_get_time() / 1000);
#else
    m_backend_hb.last_rx_ms = 0;
#endif
    m_backend_hb.valid = true;
    taskEXIT_CRITICAL(&m_hb_lock);
}

void StatisticsManager::get_backend_heartbeat(uint32_t* uptime_ms, uint32_t* rx_total, uint32_t* loop_counter, uint32_t* last_rx_ms, bool* valid) const
{
    if (!uptime_ms || !rx_total || !loop_counter || !last_rx_ms || !valid) return;
    
    taskENTER_CRITICAL(&m_hb_lock);
    *uptime_ms = m_backend_hb.uptime_ms;
    *rx_total = m_backend_hb.rx_total;
    *loop_counter = m_backend_hb.loop_counter;
    *last_rx_ms = m_backend_hb.last_rx_ms;
    *valid = m_backend_hb.valid;
    taskEXIT_CRITICAL(&m_hb_lock);
}

const char* StatisticsManager::get_packet_type_name(uint8_t packet_type) const
{
    switch (packet_type) {
        case 0x01: return "SYNC";
        case 0x02: return "CONTROL_CHANGE";
        case 0x03: return "NOTE_ON";
        case 0x04: return "NOTE_OFF";
        case 0x05: return "SAMPLE_LOAD";
        case 0x06: return "SAMPLE_DATA";
        case 0x07: return "PARAMETER_UPDATE";
        case 0x08: return "STATUS_REQUEST";
        case 0x09: return "STATUS_RESPONSE";
        case 0x0A: return "SAMPLE_CTRL";
        case 0x0B: return "PREVIEW_REQ";
        case 0x0C: return "DATA_REQUEST";
        case 0x0D: return "METER_PUSH";
        case 0x0E: return "WAVE_CHUNK";
        case 0x0F: return "HEARTBEAT";
        case 0xFF: return "ERROR";
        default: return "UNKNOWN";
    }
}

void StatisticsManager::update_tx_stats(uint8_t message_type)
{
    increment_tx_message(message_type);
}
