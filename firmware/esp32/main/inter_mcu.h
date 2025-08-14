#pragma once

#ifdef ESP_PLATFORM
#include "esp_err.h"
#else
typedef int esp_err_t;
#ifndef ESP_OK
#define ESP_OK 0
#endif
#endif

// Early init (now a lightweight stub). Real SPI bring-up occurs in inter_mcu_start().
esp_err_t inter_mcu_init(void);
// Perform SPI bus/device setup and start RX task. Safe to call once after LGFX init.
esp_err_t inter_mcu_start(void);
esp_err_t inter_mcu_send_control_change(uint8_t parameter, uint8_t channel, uint16_t value);
esp_err_t inter_mcu_send_note_on(uint8_t note, uint8_t velocity, uint8_t channel);
esp_err_t inter_mcu_send_note_off(uint8_t note, uint8_t channel); 

// Phase I helpers
typedef enum {
    WAVEX_SAMPLE_REC_START = 1,
    WAVEX_SAMPLE_REC_STOP  = 2,
    WAVEX_SAMPLE_PLAY_START = 3,
    WAVEX_SAMPLE_PLAY_STOP  = 4,
} wavex_sample_ctrl_cmd_t;

esp_err_t inter_mcu_send_sample_ctrl(uint8_t slot, wavex_sample_ctrl_cmd_t cmd, float rate);
esp_err_t inter_mcu_send_preview_req(uint8_t slot, uint32_t start, uint32_t end, uint16_t decim);

// Listener registration for backend->frontend messages
typedef void (*wavex_meter_cb_t)(float rms, float peak, void* user_data);
typedef void (*wavex_wave_chunk_cb_t)(uint32_t offset, const int16_t* samples, uint16_t count, void* user_data);

void inter_mcu_set_meter_listener(wavex_meter_cb_t cb, void* user_data);
void inter_mcu_set_wave_chunk_listener(wavex_wave_chunk_cb_t cb, void* user_data);

// Control RX task behavior
extern "C" void inter_mcu_set_suspended(bool suspended);
extern "C" bool inter_mcu_is_busy(void);