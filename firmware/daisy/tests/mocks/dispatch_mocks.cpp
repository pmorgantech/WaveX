// Implementations of the external symbols daisy_inter_mcu_message_handlers.cpp
// needs, recording every call for the dispatch host test. See dispatch_mocks.h.

#include "dispatch_mocks.h"

#include "daisy_seed.h"

// Match the real headers' declarations.
#include "audio/audio_engine.h"
#include "daisy_filesystem.h"
#include "daisy_uart_link.h"

namespace WaveX {
namespace Test {

DispatchRecord& GetDispatchRecord() {
    static DispatchRecord record;
    return record;
}

}  // namespace Test
}  // namespace WaveX

namespace WaveX {
namespace Comm {

// The handlers guard every PrintLine on s_hw; leave it null so the test
// output stays quiet and the guards take the fast path.
daisy::DaisySeed* s_hw = nullptr;

int UartLinkSend(uint16_t msg_type, const void* /*payload*/, uint16_t len) {
    WaveX::Test::GetDispatchRecord().uart_sends.push_back({msg_type, len});
    return static_cast<int>(len);
}

void DiagSubscribe(bool enable, uint8_t interval_hz) {
    WaveX::Test::GetDispatchRecord().diag_subscribes.push_back({enable, interval_hz});
}

void ProcessBrowseRequest(const char* path, size_t start_index, uint8_t max_entries) {
    WaveX::Test::GetDispatchRecord().browse_requests.push_back(
        {path ? std::string(path) : std::string(), start_index, max_entries});
}

void ProcessSamplePlayRequest(const char* file_path) {
    WaveX::Test::GetDispatchRecord().play_requests.push_back(file_path ? std::string(file_path)
                                                                       : std::string());
}

void ProcessSampleStopRequest(uint8_t slot) {
    WaveX::Test::GetDispatchRecord().stop_requests.push_back(slot);
}

void ProcessSamplePlayIndexRequest(uint32_t file_index) {
    WaveX::Test::GetDispatchRecord().play_index_requests.push_back(file_index);
}

void ProcessSampleGetPathRequest(uint32_t /*file_index*/) {}

}  // namespace Comm
}  // namespace WaveX

namespace WaveX {
namespace AudioEngine {

void SetLoopGapMs(uint16_t gap_ms) {
    WaveX::Test::GetDispatchRecord().loop_gaps_ms.push_back(gap_ms);
}

void SelectSample(uint16_t sample_id, uint8_t slot) {
    WaveX::Test::GetDispatchRecord().selected_samples.push_back({sample_id, slot});
}

uint16_t SelectedSample(uint8_t slot) {
    auto& sel = WaveX::Test::GetDispatchRecord().selected_samples;
    for (auto it = sel.rbegin(); it != sel.rend(); ++it) {
        if (it->slot == slot)
            return it->sample_id;
    }
    return 0;
}

bool UnloadSample(uint16_t sample_id) {
    WaveX::Test::GetDispatchRecord().unloaded_samples.push_back(sample_id);
    return sample_id != 0;  // mirrors the engine: 0 is rejected, not a wildcard
}

void PushAllSampleMeta(uint16_t sample_id) {
    WaveX::Test::GetDispatchRecord().meta_requests.push_back(sample_id);
}

void SetEditParams(uint8_t slot,
                   bool loop_enabled,
                   int16_t gain_db_x10,
                   uint32_t start_frame,
                   uint32_t end_frame,
                   uint32_t loop_start_frame,
                   uint32_t loop_end_frame,
                   uint16_t fade_in_ms,
                   uint16_t fade_out_ms) {
    WaveX::Test::GetDispatchRecord().sample_edits.push_back(
        WaveX::Protocol::SampleEditMessage(slot,
                                           loop_enabled ? 1 : 0,
                                           gain_db_x10,
                                           start_frame,
                                           end_frame,
                                           loop_start_frame,
                                           loop_end_frame,
                                           fade_in_ms,
                                           fade_out_ms));
}

void OnControlChange(const WaveX::Protocol::ControlChangeMessage& m) {
    WaveX::Test::GetDispatchRecord().control_changes.push_back(m);
}

void OnMixOp(const WaveX::Protocol::MixOpMessage& m) {
    WaveX::Test::GetDispatchRecord().mix_ops.push_back(m);
}

void OnNoteOn(const WaveX::Protocol::NoteMessage& m) {
    WaveX::Test::GetDispatchRecord().note_ons.push_back(m);
}

void OnNoteOff(const WaveX::Protocol::NoteMessage& m) {
    WaveX::Test::GetDispatchRecord().note_offs.push_back(m);
}

void OnSampleCtrl(const WaveX::Protocol::SampleCtrlMessage& m) {
    WaveX::Test::GetDispatchRecord().sample_ctrls.push_back(m);
}

void OnPreviewReq(const WaveX::Protocol::PreviewReqMessage& m) {
    WaveX::Test::GetDispatchRecord().preview_reqs.push_back(m);
}

void OnEnvelopeReq(const WaveX::Protocol::EnvelopeReqMessage& m) {
    WaveX::Test::GetDispatchRecord().envelope_reqs.push_back(m);
}

void OnSampleLoad(const WaveX::Protocol::SampleLoadMessage& m) {
    WaveX::Test::GetDispatchRecord().sample_loads.push_back(m);
}

void OnCvCalSet(const WaveX::Protocol::CvCalMessage& m) {
    WaveX::Test::GetDispatchRecord().cv_cal_sets.push_back(m);
}

void OnCvCalGet(const WaveX::Protocol::CvCalGetMessage& m) {
    WaveX::Test::GetDispatchRecord().cv_cal_gets.push_back(m.group);
}

void OnCvTest(const WaveX::Protocol::CvTestMessage& m) {
    WaveX::Test::GetDispatchRecord().cv_tests.push_back(m);
}

void GetSampleMemStatus(WaveX::Protocol::SampleMemStatusMessage& out) {
    out = WaveX::Protocol::SampleMemStatusMessage();
    WaveX::Test::GetDispatchRecord().get_sample_mem_status_calls++;
}

void OnSeqTransport(const WaveX::Protocol::SeqTransportMessage& m) {
    WaveX::Test::GetDispatchRecord().seq_transports.push_back(m);
}

void OnSeqPatternOp(const WaveX::Protocol::SeqPatternOpMessage& m) {
    WaveX::Test::GetDispatchRecord().seq_pattern_ops.push_back(m);
}

void OnMidiClockEvent(const WaveX::Protocol::MidiClockEventMessage& m) {
    WaveX::Test::GetDispatchRecord().midi_clock_events.push_back(m);
}

void OnMidiCc(const WaveX::Protocol::MidiCcMessage& m) {
    WaveX::Test::GetDispatchRecord().midi_ccs.push_back(m);
}

void OnInstrumentOp(const WaveX::Protocol::InstOpMessage& m) {
    WaveX::Test::GetDispatchRecord().instrument_ops.push_back(m);
}

}  // namespace AudioEngine
}  // namespace WaveX
