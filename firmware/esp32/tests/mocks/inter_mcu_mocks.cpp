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

void inter_mcu_invoke_inst_status_callback(const WaveX::Protocol::InstStatusMessage& status) {
    auto& cap = GetInterMcuCapture();
    cap.inst_status_calls++;
    cap.last_inst_status = status;
}

void inter_mcu_invoke_cv_cal_callback(const WaveX::Protocol::CvCalMessage& cal) {
    (void)cal;
    GetInterMcuCapture().cv_cal_calls++;
}

void inter_mcu_invoke_envelope_chunk_callback(const WaveX::Protocol::EnvelopeChunkMessage& header,
                                              const WaveX::Protocol::EnvelopeColumn* columns) {
    auto& cap = GetInterMcuCapture();
    cap.envelope_chunk_calls++;
    cap.envelope_header = header;
    cap.envelope_columns.assign(columns,
                                columns + static_cast<size_t>(header.columns) * header.channels);
}

void inter_mcu_store_seq_page(const WaveX::Protocol::SeqPatternSyncMessage& page) {
    auto& cap = GetInterMcuCapture();
    cap.seq_page_calls++;
    cap.last_seq_page = page;
}
void inter_mcu_store_seq_playhead(const WaveX::Protocol::SeqPlayheadMessage& playhead) {
    auto& cap = GetInterMcuCapture();
    cap.seq_playhead_calls++;
    cap.last_seq_playhead = playhead;
}

void inter_mcu_store_seq_file_status(const WaveX::Protocol::SeqFileStatusMessage& status) {
    (void)status;
}
void inter_mcu_store_project_status(const WaveX::Protocol::ProjectStatusMessage& status) {
    auto& cap = GetInterMcuCapture();
    ++cap.project_status_calls;
    cap.project_status = status;
}
void inter_mcu_store_card_state(const WaveX::Protocol::CardStateMessage&) {}

void inter_mcu_store_mix_state(const WaveX::Protocol::MixStateMessage&) {}

void inter_mcu_store_track_state(const WaveX::Protocol::TrackStateMessage& state) {
    auto& cap = GetInterMcuCapture();
    ++cap.track_state_calls;
    cap.track_state = state;
}
void inter_mcu_store_key_map(const WaveX::Protocol::InstKeyMapSyncMessage& state) {
    auto& cap = WaveX::Test::GetInterMcuCapture();
    ++cap.key_map_calls;
    cap.key_map = state;
}
void inter_mcu_store_pad_sound(const WaveX::Protocol::InstPadSoundSyncMessage& state) {
    auto& cap = WaveX::Test::GetInterMcuCapture();
    ++cap.pad_sound_calls;
    cap.pad_sound = state;
}
void inter_mcu_store_instrument_map(const WaveX::Protocol::InstZoneSyncMessage& map) {
    auto& cap = GetInterMcuCapture();
    ++cap.instrument_map_calls;
    cap.instrument_map = map;
}

void inter_mcu_store_oscillator(const WaveX::Protocol::InstOscSyncMessage& state) {
    auto& cap = GetInterMcuCapture();
    ++cap.oscillator_calls;
    cap.oscillator = state;
}

void inter_mcu_store_modulator(const WaveX::Protocol::InstModSyncMessage& state) {
    auto& cap = GetInterMcuCapture();
    ++cap.modulator_calls;
    cap.modulator = state;
}
void inter_mcu_store_instrument_lfo(const WaveX::Protocol::InstLfoSyncMessage& state) {
    auto& cap = GetInterMcuCapture();
    ++cap.instrument_lfo_calls;
    cap.instrument_lfo = state;
}

void inter_mcu_store_instrument_edit(const WaveX::Protocol::InstEditSyncMessage& state) {
    auto& cap = GetInterMcuCapture();
    ++cap.instrument_edit_calls;
    cap.instrument_edit = state;
}

void inter_mcu_store_mix_meters(const WaveX::Protocol::MixMetersMessage& message) {
    auto& cap = GetInterMcuCapture();
    ++cap.mix_meter_calls;
    cap.mix_meters = message;
}

void inter_mcu_store_seq_slot_status(const WaveX::Protocol::SeqSlotStatusMessage& status) {
    auto& cap = GetInterMcuCapture();
    ++cap.seq_slot_status_calls;
    cap.seq_slot_status = status;
}

void inter_mcu_store_seq_slot_page(const WaveX::Protocol::SeqSlotPageMessage& page) {
    auto& cap = GetInterMcuCapture();
    ++cap.seq_slot_page_calls;
    cap.seq_slot_page = page;
}
