#include "../main/inter_mcu.h"

esp_err_t inter_mcu_send_sample_load_req(uint16_t sample_id,
                                         uint32_t sample_size,
                                         uint16_t sample_rate,
                                         uint8_t channels,
                                         uint8_t bit_depth) {
    (void)sample_id;
    (void)sample_size;
    (void)sample_rate;
    (void)channels;
    (void)bit_depth;
    return ESP_OK;
}

esp_err_t inter_mcu_send_sample_data(const uint8_t* data, size_t length) {
    (void)data;
    (void)length;
    return ESP_OK;
}


