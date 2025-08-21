#pragma once

#include "daisy_seed.h"
#include "daisysp.h"
#include "spi_protocol/protocol.h"

namespace WaveX {
namespace AudioEngine {

void Init(daisy::DaisySeed& hw, float sample_rate);
void Callback(daisy::AudioHandle::InputBuffer in,
              daisy::AudioHandle::OutputBuffer out,
              size_t size);

// Control/message hook APIs
void OnControlChange(const WaveX::Protocol::ControlChangeMessage& m);
void OnNoteOn(const WaveX::Protocol::NoteMessage& m);
void OnNoteOff(const WaveX::Protocol::NoteMessage& m);
void OnSampleCtrl(const WaveX::Protocol::SampleCtrlMessage& m);
void OnPreviewReq(const WaveX::Protocol::PreviewReqMessage& m);

// Meter helpers
void GetInputMeters(float& rms, float& peak);

} // namespace AudioEngine
} // namespace WaveX


