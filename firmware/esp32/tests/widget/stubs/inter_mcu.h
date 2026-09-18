#pragma once
#include "spi_protocol/protocol.h"
using esp_err_t = int;
constexpr esp_err_t ESP_OK = 0;
bool inter_mcu_backend_link_alive();
esp_err_t inter_mcu_request_mix_state(const WaveX::Protocol::MixStateRequest&);
esp_err_t inter_mcu_send_mix_op(uint8_t, uint8_t, uint16_t);
bool inter_mcu_get_mix_state(WaveX::Protocol::MixStateMessage*);

bool inter_mcu_get_mix_meters(WaveX::Protocol::MixMetersMessage*);
esp_err_t inter_mcu_send_project_op(const WaveX::Protocol::ProjectOpMessage&);
bool inter_mcu_get_project_status(WaveX::Protocol::ProjectStatusMessage*);

esp_err_t inter_mcu_send_seq_slot_op(const WaveX::Protocol::SeqSlotOpMessage&);
bool inter_mcu_get_seq_slot_status(WaveX::Protocol::SeqSlotStatusMessage*);
esp_err_t inter_mcu_send_seq_song_op(const WaveX::Protocol::SeqSongOpMessage& request);
bool inter_mcu_get_seq_song_status(WaveX::Protocol::SeqSongStatusMessage* out);

bool inter_mcu_get_sample_meta(uint16_t, WaveX::Protocol::SampleMetadata*);
size_t inter_mcu_get_sample_meta_page(WaveX::Protocol::SampleMetadata*,
                                      size_t,
                                      uint16_t*,
                                      uint16_t*);
bool inter_mcu_get_track_binding(uint8_t, WaveX::Protocol::TrackBindingMessage*);
esp_err_t inter_mcu_request_track_binding(uint8_t);
esp_err_t inter_mcu_request_sample_meta_page(uint16_t, uint8_t);
esp_err_t inter_mcu_request_sample_mem_status();
uint32_t inter_mcu_sample_pool_revision();
uint32_t inter_mcu_sample_cache_revision();
esp_err_t inter_mcu_send_sample_select(uint16_t, uint8_t);
esp_err_t inter_mcu_send_sample_unload(uint16_t);

esp_err_t inter_mcu_send_bank_slot_op(const WaveX::Protocol::BankSlotOpMessage&);
esp_err_t inter_mcu_send_bank_op(const WaveX::Protocol::BankOpMessage&);
bool inter_mcu_get_bank_status(WaveX::Protocol::BankStatusMessage*);
