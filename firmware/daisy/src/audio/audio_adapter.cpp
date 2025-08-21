#include "audio_adapter.h"
#include "audio_engine.h"

namespace WaveX {
namespace AudioAdapter {

void HandleControlChange(const WaveX::Protocol::ControlChangeMessage& m) {
    WaveX::AudioEngine::OnControlChange(m);
}

void HandleNoteOn(const WaveX::Protocol::NoteMessage& m) {
    WaveX::AudioEngine::OnNoteOn(m);
}

void HandleNoteOff(const WaveX::Protocol::NoteMessage& m) {
    WaveX::AudioEngine::OnNoteOff(m);
}

void HandleSampleCtrl(const WaveX::Protocol::SampleCtrlMessage& m) {
    WaveX::AudioEngine::OnSampleCtrl(m);
}

void HandlePreviewReq(const WaveX::Protocol::PreviewReqMessage& m) {
    WaveX::AudioEngine::OnPreviewReq(m);
}

} // namespace AudioAdapter
} // namespace WaveX


