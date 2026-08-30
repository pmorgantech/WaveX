#include "../main/comm/packet_router.h"
#include "../main/inter_mcu.h"
#include "esp32_mocks.h"

using WaveX::Test::GetInterMcuCapture;

// esp_uart_link.cpp (real UART/FreeRTOS driver code) isn't compiled into the
// host test libraries; application_context.cpp still calls this at
// construction time, so provide a no-op stand-in for host tests.
void uart_link_set_packet_router(WaveX::Comm::PacketRouter* packet_router) {
    (void)packet_router;
}

esp_err_t inter_mcu_send_sample_load_req(uint16_t sample_id,
                                         uint32_t sample_size,
                                         uint16_t sample_rate,
                                         uint8_t channels,
                                         uint8_t bit_depth,
                                         const char* path) {
    (void)sample_id;
    (void)sample_size;
    (void)sample_rate;
    (void)channels;
    (void)bit_depth;
    (void)path;
    return ESP_OK;
}

esp_err_t inter_mcu_send_sample_data(const uint8_t* data, size_t length) {
    auto& cap = GetInterMcuCapture();
    cap.sample_data_calls++;
    cap.sample_data.assign(data, data + length);
    return cap.send_result;
}

void inter_mcu_update_sample_mem_status(const wavex_sample_mem_status_t& status) {
    (void)status;
    GetInterMcuCapture().sample_mem_status_calls++;
}

void inter_mcu_invoke_sample_status_callback(uint16_t sample_id,
                                             uint8_t state,
                                             uint32_t sample_rate,
                                             uint8_t channels,
                                             uint32_t frames_played) {
    auto& cap = GetInterMcuCapture();
    cap.sample_status_calls++;
    cap.sample_status_id = sample_id;
    cap.sample_status_state = state;
    cap.sample_status_rate = sample_rate;
    cap.sample_status_channels = channels;
    cap.sample_status_frames = frames_played;
}

void inter_mcu_invoke_cv_cal_callback(const WaveX::Protocol::CvCalMessage& cal) {
    (void)cal;
    GetInterMcuCapture().cv_cal_calls++;
}

void inter_mcu_invoke_wave_chunk_callback(uint32_t offset, const int16_t* samples, uint16_t count) {
    auto& cap = GetInterMcuCapture();
    cap.wave_chunk_calls++;
    cap.wave_chunk_offset = offset;
    cap.wave_chunk_samples.assign(samples, samples + count);
}

void inter_mcu_invoke_envelope_chunk_callback(const WaveX::Protocol::EnvelopeChunkMessage& header,
                                              const WaveX::Protocol::EnvelopeColumn* columns) {
    auto& cap = GetInterMcuCapture();
    cap.envelope_chunk_calls++;
    cap.envelope_header = header;
    cap.envelope_columns.assign(columns,
                                columns + static_cast<size_t>(header.columns) * header.channels);
}
