/**
 * @file inter_mcu.h
 * @brief High-level API for communicating with the Daisy backend over the active link (UART).
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "../../shared/spi_protocol/protocol.h"
#include "comm/statistics.h"
#ifdef ESP_PLATFORM
#include "esp_err.h"
#else
typedef int esp_err_t;
#ifndef ESP_OK
#define ESP_OK 0
#endif
#endif

class StatisticsManager;

esp_err_t inter_mcu_init(StatisticsManager& statistics);
esp_err_t inter_mcu_start(void);

// CV calibration workflow (roadmap item 5 stage 5; analog-voice-board.md §3)
esp_err_t inter_mcu_send_cv_cal_set(const WaveX::Protocol::CvCalMessage& cal);
esp_err_t inter_mcu_send_cv_cal_get(uint8_t group);
esp_err_t inter_mcu_send_cv_test(const WaveX::Protocol::CvTestMessage& test);
// Fired from the UART task when MSG_CV_CAL_RESP arrives - do NOT touch
// LVGL in the callback (deferred-update pattern, ui-architecture.md).
typedef void (*wavex_cv_cal_cb_t)(const WaveX::Protocol::CvCalMessage& cal, void* user_data);
void inter_mcu_set_cv_cal_listener(wavex_cv_cal_cb_t cb, void* user_data);
void inter_mcu_invoke_cv_cal_callback(const WaveX::Protocol::CvCalMessage& cal);

// Basic MIDI message sending
esp_err_t inter_mcu_send_control_change(uint8_t parameter, uint8_t channel, uint16_t value);

/**
 * Note senders, split by how the note is addressed
 * (track-and-patch-model.md §2.2; NOTE_ADDR_TRACK in protocol.h).
 *
 * `_midi_` forwards an event that arrived on a MIDI port with its channel
 * unchanged; the backend routes it to every Track listening on that channel,
 * so one channel can reach several Tracks (a layer). Only the MIDI readers
 * call these.
 *
 * `_track_` addresses one Track directly - the Play grid, the sequencer, an
 * audition. There is no channel involved and no routing to do.
 *
 * Two functions rather than one with a flag argument because the caller
 * always knows which it means, and a bool at the call site is exactly the
 * kind of thing that gets passed the wrong way round.
 */
esp_err_t inter_mcu_send_note_on_midi(uint8_t note, uint8_t velocity, uint8_t channel);
esp_err_t inter_mcu_send_note_off_midi(uint8_t note, uint8_t channel);
esp_err_t inter_mcu_send_note_on_track(uint8_t note, uint8_t velocity, uint8_t track);
esp_err_t inter_mcu_send_note_off_track(uint8_t note, uint8_t track);

/**
 * One Track setting (MSG_TRACK_OP). `op` is a WaveX::Protocol::TrackOp and
 * `value` is op-dependent - see protocol.h, where each op states its
 * encoding.
 */
esp_err_t inter_mcu_send_track_op(uint8_t op, uint8_t track, uint16_t value);

// Phase I helpers
typedef enum {
    WAVEX_SAMPLE_REC_START = 1,
    WAVEX_SAMPLE_REC_STOP = 2,
    WAVEX_SAMPLE_PLAY_START = 3,
    WAVEX_SAMPLE_PLAY_STOP = 4,
} wavex_sample_ctrl_cmd_t;

esp_err_t inter_mcu_send_sample_ctrl(uint8_t slot, wavex_sample_ctrl_cmd_t cmd, float rate);
esp_err_t inter_mcu_send_preview_req(uint8_t slot, uint32_t start, uint32_t end, uint16_t decim);

// Min/max waveform envelope for a frame window (roadmap 1.5.5 item 2). Unlike
// the decimated preview above, the reply's size follows the requested column
// count rather than the file length, and it does not alias.
// sample_id 0 = the most recently loaded sample; end_frame 0 = to the end.
esp_err_t inter_mcu_send_envelope_req(uint16_t sample_id,
                                      uint16_t columns,
                                      uint32_t start_frame,
                                      uint32_t end_frame);

// Non-destructive playback edit (MSG_SAMPLE_EDIT_SET). Frames are absolute at
// the file's own rate; 0 means "to the end" for end_frame and loop_end. The
// backend clamps and is the authority - do not assume the values were taken
// verbatim.
esp_err_t inter_mcu_send_sample_edit(uint8_t slot,
                                     bool loop_enabled,
                                     int16_t gain_db_x10,
                                     uint32_t start_frame,
                                     uint32_t end_frame,
                                     uint32_t loop_start,
                                     uint32_t loop_end,
                                     uint16_t fade_in_ms,
                                     uint16_t fade_out_ms);

// Per-sample metadata cache (MSG_SAMPLE_META). The Daisy is authoritative and
// pushes on every change; the frontend never derives these values.
void inter_mcu_store_sample_meta(const WaveX::Protocol::SampleMetadata& msg);

/** Newest record for an id, or the most recent record when sample_id is 0. */
bool inter_mcu_get_sample_meta(uint16_t sample_id, WaveX::Protocol::SampleMetadata* out);
// Every cached record, in cache order. Returns how many were written.
size_t inter_mcu_sample_meta_snapshot(WaveX::Protocol::SampleMetadata* out, size_t max);

// The Sample Pool is paged, not mirrored: ask for a window of resident
// records in registry order and read the last page that arrived. The page
// is one frame from the Daisy (MSG_SAMPLE_META_PAGE); its records also land
// in the per-id cache above.
esp_err_t inter_mcu_request_sample_meta_page(uint16_t first, uint8_t count);
void inter_mcu_store_sample_meta_page(const WaveX::Protocol::SampleMetaPageHeader& header,
                                      const uint8_t* records);
// Copies the last page: returns records written; `total` and `first` are
// the Pool's count and the page's start as the Daisy reported them.
size_t inter_mcu_get_sample_meta_page(WaveX::Protocol::SampleMetadata* out,
                                      size_t max,
                                      uint16_t* total,
                                      uint16_t* first);

/** Ask the backend to resend. sample_id 0 = every loaded sample. */
esp_err_t inter_mcu_request_sample_meta(uint16_t sample_id);

/// Backend-authoritative binding for one Track (0..15), or all Tracks when
/// `track` is 0xFF. Unlike page-local Select history, this also reports SFZ
/// Patches and selection requests the backend refused.
esp_err_t inter_mcu_request_track_binding(uint8_t track);
void inter_mcu_store_track_binding(const WaveX::Protocol::TrackBindingMessage& msg);
bool inter_mcu_get_track_binding(uint8_t track, WaveX::Protocol::TrackBindingMessage* out);

/// Binds `sample_id` for note-on playback on `slot` (0..15, matches
/// inter_mcu_send_note_on's channel). sample_id 0 clears that slot's
/// binding - its note-on then drops rather than falling back to any other
/// slot's sample.
esp_err_t inter_mcu_send_sample_select(uint16_t sample_id, uint8_t slot);

/// Frees a loaded sample's RAM on the backend. Sounding voices are stopped
/// first. sample_id 0 is rejected by the backend rather than treated as "all".
esp_err_t inter_mcu_send_sample_unload(uint16_t sample_id);

// Diagnostics telemetry (MSG_DIAG_SUBSCRIBE / MSG_DIAG_PUSH). Subscribe only
// while the diagnostics page is open; the backend sends nothing otherwise.
esp_err_t inter_mcu_send_diag_subscribe(bool enable, uint8_t interval_hz);

// Called by the packet router on each push. Not for application use.
void inter_mcu_store_diag_push(const WaveX::Protocol::DiagPushMessage& msg);

// Latest telemetry. Returns false if no push has arrived, or if the most
// recent one is older than max_age_ms - a stale figure presented as current is
// how a dead link reads as a healthy one.
bool inter_mcu_get_diag_push(WaveX::Protocol::DiagPushMessage* out, uint32_t max_age_ms);

// Listener registration for backend->frontend messages
typedef void (*wavex_meter_cb_t)(
    float rms_left, float rms_right, float peak_left, float peak_right, void* user_data);
typedef void (*wavex_wave_chunk_cb_t)(uint32_t offset,
                                      const int16_t* samples,
                                      uint16_t count,
                                      void* user_data);
// One run of envelope columns (MSG_ENVELOPE_CHUNK). The header carries the
// window, the generation and the channel count, so a listener can decide
// whether a chunk still matters without keeping request state. `columns` is
// valid only for the duration of the call - it points into the RX buffer.
typedef void (*wavex_envelope_chunk_cb_t)(const WaveX::Protocol::EnvelopeChunkMessage& header,
                                          const WaveX::Protocol::EnvelopeColumn* columns,
                                          void* user_data);
typedef void (*wavex_browse_resp_cb_t)(const uint8_t* data, size_t length, void* user_data);
typedef void (*wavex_sample_status_cb_t)(uint16_t sample_id,
                                         uint8_t state,
                                         uint32_t sample_rate,
                                         uint8_t channels,
                                         uint32_t frames_played,
                                         void* user_data);
typedef void (*wavex_inst_status_cb_t)(const WaveX::Protocol::InstStatusMessage& status,
                                       void* user_data);

void inter_mcu_set_wave_chunk_listener(wavex_wave_chunk_cb_t cb, void* user_data);
void inter_mcu_set_envelope_chunk_listener(wavex_envelope_chunk_cb_t cb, void* user_data);
void inter_mcu_invoke_envelope_chunk_callback(const WaveX::Protocol::EnvelopeChunkMessage& header,
                                              const WaveX::Protocol::EnvelopeColumn* columns);
void inter_mcu_invoke_browse_resp_callback(const uint8_t* data, size_t length);
void inter_mcu_invoke_storage_status_callback(bool mounted);
void inter_mcu_invoke_wave_chunk_callback(uint32_t offset, const int16_t* samples, uint16_t count);
void inter_mcu_set_sample_status_listener(wavex_sample_status_cb_t cb, void* user_data);
void inter_mcu_invoke_sample_status_callback(uint16_t sample_id,
                                             uint8_t state,
                                             uint32_t sample_rate,
                                             uint8_t channels,
                                             uint32_t frames_played);
void inter_mcu_set_inst_status_listener(wavex_inst_status_cb_t cb, void* user_data);
void inter_mcu_invoke_inst_status_callback(const WaveX::Protocol::InstStatusMessage& status);

void inter_mcu_handle_sample_stop_response(bool success);

// Request/command functions sent to the backend over the active link (replaces
// the earlier LinkManager abstraction).
esp_err_t inter_mcu_send_browse_req(const char* path, uint8_t start_index);
// loop_gap_ms: silence between loop passes. The sample browser passes ~300 so
// a short file does not sound like a drone; the editor passes 0 so the loop
// seam is heard exactly as it will play.
esp_err_t inter_mcu_send_sample_play_index_req(uint32_t file_index, uint16_t loop_gap_ms = 0);
esp_err_t inter_mcu_send_sample_stop_req();
esp_err_t inter_mcu_send_sample_load_req(uint16_t sample_id,
                                         uint32_t sample_size,
                                         uint16_t sample_rate,
                                         uint8_t channels,
                                         uint8_t bit_depth,
                                         const char* path);
esp_err_t inter_mcu_send_sample_data(const uint8_t* data, size_t length);
esp_err_t inter_mcu_send_inst_op(uint32_t request_id,
                                 uint8_t slot,
                                 WaveX::Protocol::InstOpCode op,
                                 const char* path);
// INST_OP_SET_MOD_SLOT (param-locks-and-modulation.md §9 stage 4). source/
// dest/curve/flags are WaveX::AudioEngine::ModSource/ModDest/ModCurve/
// ModSlotFlags values (mod_matrix.hpp) - that header is Daisy audio-engine
// code the ESP32 side does not build against, so this takes the raw wire
// bytes directly, the same way MidiCcMessage's `cc` is a raw byte here.
esp_err_t inter_mcu_send_mod_slot(uint32_t request_id,
                                  uint8_t instrument_slot,
                                  uint8_t mod_slot_index,
                                  uint8_t source,
                                  uint8_t dest,
                                  int16_t depth,
                                  uint8_t curve,
                                  uint8_t flags);

// Control RX task behavior
extern "C" void inter_mcu_set_suspended(bool suspended);
extern "C" bool inter_mcu_is_busy(void);

// Backend heartbeat diagnostics (from Daisy)
typedef struct {
    uint32_t uptime_ms;
    uint32_t rx_total;
    uint32_t loop_counter;
    uint32_t last_rx_ms;      // esp_timer (ms) when last heartbeat was received
    float cpu_usage_percent;  // CPU usage percentage from Daisy (legacy)
    float cpu_avg_percent;
    float cpu_min_percent;
    float cpu_max_percent;
    bool valid;
} wavex_backend_heartbeat_t;

using wavex_sample_mem_entry_t = WaveX::Protocol::SampleMemEntryMessage;
using wavex_sample_mem_status_t = WaveX::Protocol::SampleMemStatusMessage;

// Thread-safe snapshot of latest heartbeat
void inter_mcu_get_backend_heartbeat(wavex_backend_heartbeat_t* out);

// Thread-safe snapshot of latest heartbeat with detailed CPU metrics
void inter_mcu_get_backend_heartbeat_detailed(wavex_backend_heartbeat_t* out);

// Thread-safe snapshot of current packet statistics
void inter_mcu_get_packet_stats(wavex_packet_stats_t* out);

void inter_mcu_reset_packet_stats(void);

void inter_mcu_get_packet_summary(wavex_packet_summary_t* out);

uint32_t inter_mcu_get_meter_packet_count(void);

uint32_t inter_mcu_get_total_packet_count(void);

// Sample memory diagnostics
esp_err_t inter_mcu_request_sample_mem_status();
void inter_mcu_get_sample_mem_status(wavex_sample_mem_status_t* out);
void inter_mcu_update_sample_mem_status(const wavex_sample_mem_status_t& status);

// Returns the number of characters written (excluding null terminator)
int inter_mcu_format_packet_stats(char* buffer, size_t buffer_size);

void inter_mcu_get_tx_stats(wavex_tx_stats_t* out);

void inter_mcu_update_backend_heartbeat(uint32_t uptime_ms,
                                        uint32_t rx_total,
                                        uint32_t loop_counter,
                                        float cpu_usage_percent);

// Called by the packet router as it decodes MSG_HEARTBEAT off the link.
void inter_mcu_update_backend_heartbeat_detailed(uint32_t uptime_ms,
                                                 uint32_t rx_total,
                                                 uint32_t loop_counter,
                                                 float cpu_avg_percent,
                                                 float cpu_min_percent,
                                                 float cpu_max_percent);

// Called by the packet router as it decodes MSG_METER_PUSH off the link.
void inter_mcu_update_backend_meters(float rms_left,
                                     float rms_right,
                                     float peak_left,
                                     float peak_right);

void inter_mcu_get_meter_data(wavex_meter_data_t* out);

// Called by both link backends as each packet is classified.
void inter_mcu_increment_packet_stat(uint8_t packet_type);
