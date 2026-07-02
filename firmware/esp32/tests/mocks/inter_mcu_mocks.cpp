#include "../main/comm/packet_router.h"
#include "../main/inter_mcu.h"

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
    (void)data;
    (void)length;
    return ESP_OK;
}

void inter_mcu_update_sample_mem_status(const wavex_sample_mem_status_t& status) {
    (void)status;
}

void inter_mcu_invoke_sample_status_callback(uint16_t sample_id,
                                             uint8_t state,
                                             uint32_t sample_rate,
                                             uint8_t channels,
                                             uint32_t frames_played) {
    (void)sample_id;
    (void)state;
    (void)sample_rate;
    (void)channels;
    (void)frames_played;
}

void inter_mcu_invoke_wave_chunk_callback(uint32_t offset, const int16_t* samples, uint16_t count) {
    (void)offset;
    (void)samples;
    (void)count;
}
