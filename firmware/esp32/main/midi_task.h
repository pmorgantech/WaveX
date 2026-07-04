/**
 * @file midi_task.h
 * @brief DIN MIDI input task (roadmap Phase 1 item 8)
 *
 * Reads the 31250-baud serial MIDI stream on UART2 (pins/baud in
 * pin_config.h), parses it with the shared WaveX::Midi::StreamParser, and
 * forwards note on/off events to the Daisy backend over the inter-MCU
 * link (MSG_NOTE_ON/MSG_NOTE_OFF).
 *
 * Compiled out entirely when WAVEX_ESP_DIN_MIDI_ENABLED is 0 - the init
 * function then exists as a no-op returning ESP_OK.
 */

#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Install the MIDI UART driver and start the reader task.
 *
 * Requires inter_mcu_init()/inter_mcu_start() to have run - forwarded
 * notes are dropped (with a log) until the link is up.
 *
 * @return ESP_OK on success (also when the feature is compiled out)
 */
esp_err_t midi_task_start(void);

/**
 * @brief Stop the reader task and remove the UART driver.
 *
 * @return ESP_OK on success
 */
esp_err_t midi_task_stop(void);

#ifdef __cplusplus
}
#endif
