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
