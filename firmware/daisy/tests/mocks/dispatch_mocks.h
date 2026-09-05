#ifndef WAVEX_DISPATCH_MOCKS_H
#define WAVEX_DISPATCH_MOCKS_H

// Recording mocks for the message-dispatch host test
// (unit/comm/message_dispatch_test.cpp). dispatch_mocks.cpp provides the
// AudioEngine::On* / Comm::Process* / Comm::UartLinkSend symbols that
// daisy_inter_mcu_message_handlers.cpp links against, and records every
// call here so the test can assert that a wire message type actually
// reached its subsystem (code review C1: the note/control/sample-ctrl
// handlers were silent stubs and nothing could catch that).

#include "spi_protocol/protocol.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace WaveX {
namespace Test {

struct DispatchRecord {
    std::vector<WaveX::Protocol::NoteMessage> note_ons;
    std::vector<std::pair<uint16_t, uint8_t>> meta_page_requests;  // MSG_SAMPLE_META_PAGE_REQ
    std::vector<WaveX::Protocol::NoteMessage> note_offs;
    std::vector<WaveX::Protocol::ControlChangeMessage> control_changes;
    std::vector<WaveX::Protocol::MixOpMessage> mix_ops;
    std::vector<WaveX::Protocol::TrackOpMessage> track_ops;
    std::vector<WaveX::Protocol::SampleCtrlMessage> sample_ctrls;
    std::vector<WaveX::Protocol::PreviewReqMessage> preview_reqs;
    std::vector<WaveX::Protocol::EnvelopeReqMessage> envelope_reqs;
    std::vector<WaveX::Protocol::CvCalMessage> cv_cal_sets;
    std::vector<uint8_t> cv_cal_gets;
    std::vector<WaveX::Protocol::CvTestMessage> cv_tests;
    std::vector<WaveX::Protocol::SampleLoadMessage> sample_loads;
    std::vector<WaveX::Protocol::SampleSelectMessage> selected_samples;
    std::vector<uint8_t> track_binding_requests;
    std::vector<uint16_t> unloaded_samples;
    int get_sample_mem_status_calls = 0;

    std::vector<WaveX::Protocol::SeqTransportMessage> seq_transports;
    std::vector<WaveX::Protocol::SeqPatternOpMessage> seq_pattern_ops;
    std::vector<WaveX::Protocol::MidiClockEventMessage> midi_clock_events;
    std::vector<WaveX::Protocol::MidiCcMessage> midi_ccs;
    std::vector<WaveX::Protocol::InstOpMessage> instrument_ops;

    struct BrowseCall {
        std::string path;
        size_t start_index;
        uint8_t max_entries;
    };
    std::vector<BrowseCall> browse_requests;
    std::vector<std::string> play_requests;
    std::vector<uint8_t> stop_requests;
    std::vector<uint32_t> play_index_requests;

    std::vector<WaveX::Protocol::SampleEditMessage> sample_edits;
    std::vector<uint16_t> meta_requests;
    std::vector<uint16_t> loop_gaps_ms;

    struct DiagSubscribeCall {
        bool enable;
        uint8_t interval_hz;
    };
    std::vector<DiagSubscribeCall> diag_subscribes;

    struct UartSend {
        uint16_t msg_type;
        uint16_t len;
    };
    std::vector<UartSend> uart_sends;

    void Clear() { *this = DispatchRecord{}; }

    // Total observable subsystem calls across every recorded kind. The
    // per-type tests assert on specific vectors; the malformed-payload sweep
    // needs the aggregate, because its assertion is "an undersized payload
    // reaches NO handler" without caring which one it would have reached.
    // Anything added above must be added here too, or the sweep silently
    // stops covering it. uart_sends is deliberately NOT counted: it records
    // replies rather than subsystem dispatches, and a handler is entitled to
    // answer a malformed request with an error frame.
    size_t TotalCalls() const {
        return note_ons.size() + note_offs.size() + control_changes.size() + sample_ctrls.size() +
               preview_reqs.size() + envelope_reqs.size() + cv_cal_sets.size() +
               cv_cal_gets.size() + cv_tests.size() + sample_loads.size() +
               selected_samples.size() + track_binding_requests.size() + unloaded_samples.size() +
               static_cast<size_t>(get_sample_mem_status_calls) + seq_transports.size() +
               seq_pattern_ops.size() + midi_clock_events.size() + midi_ccs.size() +
               instrument_ops.size() + browse_requests.size() + play_requests.size() +
               stop_requests.size() + play_index_requests.size() + sample_edits.size() +
               meta_requests.size() + loop_gaps_ms.size() + diag_subscribes.size() +
               mix_ops.size() + track_ops.size();
    }
};

DispatchRecord& GetDispatchRecord();

}  // namespace Test
}  // namespace WaveX

#endif  // WAVEX_DISPATCH_MOCKS_H
