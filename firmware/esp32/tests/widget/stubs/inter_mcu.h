#pragma once
#include "spi_protocol/protocol.h"
using esp_err_t = int;
constexpr esp_err_t ESP_OK = 0;
bool inter_mcu_backend_link_alive();
esp_err_t inter_mcu_request_mix_state(const WaveX::Protocol::MixStateRequest&);
esp_err_t inter_mcu_send_mix_op(uint8_t, uint8_t, uint16_t);
bool inter_mcu_get_mix_state(WaveX::Protocol::MixStateMessage*);
