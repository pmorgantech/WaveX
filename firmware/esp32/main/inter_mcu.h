#pragma once

#include "esp_err.h"

esp_err_t inter_mcu_init(void);
esp_err_t inter_mcu_send_control_change(uint8_t parameter, uint8_t channel, uint16_t value);
esp_err_t inter_mcu_send_note_on(uint8_t note, uint8_t velocity, uint8_t channel);
esp_err_t inter_mcu_send_note_off(uint8_t note, uint8_t channel); 