#pragma once

#include "../config.hpp"
#if WAVEX_AUDIO_ENGINE_ENABLED

#include "spi_protocol/protocol.h"

namespace WaveX {
namespace AudioAdapter {

void HandleControlChange(const WaveX::Protocol::ControlChangeMessage& m);
void HandleNoteOn(const WaveX::Protocol::NoteMessage& m);
void HandleNoteOff(const WaveX::Protocol::NoteMessage& m);
void HandleSampleCtrl(const WaveX::Protocol::SampleCtrlMessage& m);
void HandlePreviewReq(const WaveX::Protocol::PreviewReqMessage& m);

} // namespace AudioAdapter
} // namespace WaveX


#endif // WAVEX_AUDIO_ENGINE_ENABLED