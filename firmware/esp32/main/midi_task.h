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

/**
 * @brief Receive-channel filter applied to incoming notes (DIN and USB).
 *
 * @param channel 0 = Omni (accept every channel), 1..16 = that channel only.
 *                Out-of-range values are ignored.
 *
 * Only Note On is filtered. Note Off is always forwarded, whatever the
 * filter says: dropping a note-off because the filter moved between press
 * and release would strand a voice sounding with nothing left to stop it.
 *
 * Callable from any task (the value is a relaxed atomic read on the MIDI
 * path); today it is set from the UI task by Settings > MIDI.
 */
void midi_set_input_channel(int channel);

/**
 * @brief Current receive-channel filter. 0 = Omni, 1..16 = that channel.
 */
int midi_get_input_channel(void);

#ifdef __cplusplus
}

#include "midi/midi_stream_parser.hpp"

/**
 * @brief Forward a parsed MIDI event to the Daisy over the inter-MCU link.
 *
 * Shared by the DIN reader here and the USB MIDI reader
 * (usb_midi_task.cpp) so both transports get identical note handling.
 * Thread-safe (the underlying link send path is mutex-protected).
 */
void midi_forward_event(const WaveX::Midi::Event& ev);
#endif
