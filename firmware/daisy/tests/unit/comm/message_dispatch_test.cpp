// Dispatch-level test: every wire message type routed by
// ProcessInterMcuMessage must produce its observable subsystem call.
//
// Why this exists (code review C1): the note/control/sample-ctrl handlers
// shipped as log-only stubs while the engine side was complete and
// unit-tested - green CI, broken product. Unit tests of VoiceManager can't
// catch that; only a test that drives the REAL dispatcher against recording
// mocks can. If a handler regresses to a stub, the corresponding test here
// fails.
//
// The real daisy_inter_mcu_message_handlers.cpp is compiled into this test
// (see tests/CMakeLists.txt); all of its external dependencies are recording
// mocks (mocks/dispatch_mocks.cpp).

#include <gtest/gtest.h>

#include "../../mocks/dispatch_mocks.h"
#include "daisy_inter_mcu_message_handlers.h"
#include "spi_protocol/protocol.h"

#include <cstring>

using namespace WaveX::Protocol;
using WaveX::Comm::ProcessInterMcuMessage;
using WaveX::Test::DispatchRecord;
using WaveX::Test::GetDispatchRecord;

namespace {

class MessageDispatchTest : public ::testing::Test {
   protected:
    void SetUp() override { GetDispatchRecord().Clear(); }

    // Serialize a packed message struct into a payload byte buffer, exactly
    // as it arrives from ParseUartPacket.
    template <typename T>
    void Dispatch(uint8_t msg_type, const T& msg, uint16_t seq = 1) {
        uint8_t payload[sizeof(T)];
        std::memcpy(payload, &msg, sizeof(T));
        ProcessInterMcuMessage(msg_type, seq, payload, sizeof(T));
    }
};

TEST_F(MessageDispatchTest, NoteOnReachesAudioEngine) {
    NoteMessage note(60, 100, 2);
    Dispatch(MSG_NOTE_ON, note);

    ASSERT_EQ(GetDispatchRecord().note_ons.size(), 1u);
    EXPECT_EQ(GetDispatchRecord().note_ons[0].note, 60);
    EXPECT_EQ(GetDispatchRecord().note_ons[0].velocity, 100);
    EXPECT_EQ(GetDispatchRecord().note_ons[0].channel, 2);
    EXPECT_TRUE(GetDispatchRecord().note_offs.empty());
}

TEST_F(MessageDispatchTest, NoteOffReachesAudioEngine) {
    NoteMessage note(72, 0, 5);
    Dispatch(MSG_NOTE_OFF, note);

    ASSERT_EQ(GetDispatchRecord().note_offs.size(), 1u);
    EXPECT_EQ(GetDispatchRecord().note_offs[0].note, 72);
    EXPECT_EQ(GetDispatchRecord().note_offs[0].channel, 5);
    EXPECT_TRUE(GetDispatchRecord().note_ons.empty());
}

TEST_F(MessageDispatchTest, ControlChangeReachesAudioEngine) {
    ControlChangeMessage cc(PARAM_FILTER_CUTOFF, 1, 0x1234);
    Dispatch(MSG_CONTROL_CHANGE, cc);

    ASSERT_EQ(GetDispatchRecord().control_changes.size(), 1u);
    EXPECT_EQ(GetDispatchRecord().control_changes[0].parameter, PARAM_FILTER_CUTOFF);
    EXPECT_EQ(GetDispatchRecord().control_changes[0].value, 0x1234);
}

TEST_F(MessageDispatchTest, SampleCtrlReachesAudioEngine) {
    SampleCtrlMessage ctrl(0, SAMPLE_REC_START, 1.0f);
    Dispatch(MSG_SAMPLE_CTRL, ctrl);

    ASSERT_EQ(GetDispatchRecord().sample_ctrls.size(), 1u);
    EXPECT_EQ(GetDispatchRecord().sample_ctrls[0].cmd, SAMPLE_REC_START);
    EXPECT_FLOAT_EQ(GetDispatchRecord().sample_ctrls[0].rate, 1.0f);
}

TEST_F(MessageDispatchTest, PreviewReqReachesAudioEngine) {
    PreviewReqMessage req(0, 100, 200, 4);
    Dispatch(MSG_PREVIEW_REQ, req);

    ASSERT_EQ(GetDispatchRecord().preview_reqs.size(), 1u);
    EXPECT_EQ(GetDispatchRecord().preview_reqs[0].start, 100u);
    EXPECT_EQ(GetDispatchRecord().preview_reqs[0].end, 200u);
    EXPECT_EQ(GetDispatchRecord().preview_reqs[0].decim, 4);
}

TEST_F(MessageDispatchTest, EnvelopeReqReachesAudioEngine) {
    EnvelopeReqMessage req(7, 1256, 44100, 7000000);
    Dispatch(MSG_ENVELOPE_REQ, req);

    ASSERT_EQ(GetDispatchRecord().envelope_reqs.size(), 1u);
    EXPECT_EQ(GetDispatchRecord().envelope_reqs[0].sample_id, 7);
    EXPECT_EQ(GetDispatchRecord().envelope_reqs[0].columns, 1256);
    EXPECT_EQ(GetDispatchRecord().envelope_reqs[0].start_frame, 44100u);
    EXPECT_EQ(GetDispatchRecord().envelope_reqs[0].end_frame, 7000000u);
}

// The edit message is the only thing that makes the sample-edit page's
// markers audible, so a silent dispatcher stub here would look exactly like
// "loop doesn't work" - which is how it presented before this path existed.
TEST_F(MessageDispatchTest, SampleEditReachesAudioEngine) {
    SampleEditMessage edit(0, 1, -35, 44100, 396900, 88200, 352800, 3, 250);
    Dispatch(MSG_SAMPLE_EDIT_SET, edit);

    ASSERT_EQ(GetDispatchRecord().sample_edits.size(), 1u);
    const auto& got = GetDispatchRecord().sample_edits[0];
    EXPECT_EQ(got.loop_enabled, 1);
    EXPECT_EQ(got.gain_db_x10, -35);
    EXPECT_EQ(got.start_frame, 44100u);
    EXPECT_EQ(got.end_frame, 396900u);
    EXPECT_EQ(got.loop_start, 88200u);
    EXPECT_EQ(got.loop_end, 352800u);
    // The fades ride on the same command, so a dispatcher that drops them
    // would present as "de-click does nothing" with everything else working.
    EXPECT_EQ(got.fade_in_ms, 3);
    EXPECT_EQ(got.fade_out_ms, 250);
}

// sample_id 0 means "every loaded sample" - the frontend's way of
// repopulating after its own restart without the backend tracking who has
// seen what. A dispatcher that dropped this would leave the UI permanently
// showing whatever it had cached.
// The loop gap rides on the play request, not on the sample's record,
// because it belongs to THIS audition: the browser wants a gap so a short
// file does not read as a drone, the editor wants none so the seam is heard
// as it will play. A dispatcher that dropped it would silently give both the
// same behaviour.
TEST_F(MessageDispatchTest, SamplePlayIndexCarriesTheLoopGap) {
    SamplePlayIndexMessage play(4, 300);
    Dispatch(MSG_SAMPLE_PLAY_INDEX_REQ, play);

    ASSERT_EQ(GetDispatchRecord().loop_gaps_ms.size(), 1u);
    EXPECT_EQ(GetDispatchRecord().loop_gaps_ms[0], 300);
    ASSERT_EQ(GetDispatchRecord().play_index_requests.size(), 1u);
    EXPECT_EQ(GetDispatchRecord().play_index_requests[0], 4u);
}

TEST_F(MessageDispatchTest, SamplePlayIndexDefaultsToGapless) {
    SamplePlayIndexMessage play(4);
    Dispatch(MSG_SAMPLE_PLAY_INDEX_REQ, play);

    ASSERT_EQ(GetDispatchRecord().loop_gaps_ms.size(), 1u);
    EXPECT_EQ(GetDispatchRecord().loop_gaps_ms[0], 0);
}

TEST_F(MessageDispatchTest, SampleMetaRequestReachesAudioEngine) {
    SampleMetaReqMessage req(0);
    Dispatch(MSG_SAMPLE_META_REQ, req);

    ASSERT_EQ(GetDispatchRecord().meta_requests.size(), 1u);
    EXPECT_EQ(GetDispatchRecord().meta_requests[0], 0);
}

TEST_F(MessageDispatchTest, SampleMetaRequestCanNameOneSample) {
    SampleMetaReqMessage req(9);
    Dispatch(MSG_SAMPLE_META_REQ, req);

    ASSERT_EQ(GetDispatchRecord().meta_requests.size(), 1u);
    EXPECT_EQ(GetDispatchRecord().meta_requests[0], 9);
}

TEST_F(MessageDispatchTest, DiagSubscribeReachesTelemetry) {
    DiagSubscribeMessage sub(1, 4);
    Dispatch(MSG_DIAG_SUBSCRIBE, sub);

    ASSERT_EQ(GetDispatchRecord().diag_subscribes.size(), 1u);
    EXPECT_TRUE(GetDispatchRecord().diag_subscribes[0].enable);
    EXPECT_EQ(GetDispatchRecord().diag_subscribes[0].interval_hz, 4);
}

// Unsubscribing must reach the backend too: without it the push keeps flowing
// after the diagnostics page closes, which is the whole cost the subscription
// exists to avoid.
TEST_F(MessageDispatchTest, DiagUnsubscribeReachesTelemetry) {
    DiagSubscribeMessage sub(0, 2);
    Dispatch(MSG_DIAG_SUBSCRIBE, sub);

    ASSERT_EQ(GetDispatchRecord().diag_subscribes.size(), 1u);
    EXPECT_FALSE(GetDispatchRecord().diag_subscribes[0].enable);
}

TEST_F(MessageDispatchTest, SampleLoadReachesAudioEngine) {
    SampleLoadMessage load(7, 44100, 44100, 1, 16, "/SAMPLES/kick.wav");
    Dispatch(MSG_SAMPLE_LOAD, load);

    ASSERT_EQ(GetDispatchRecord().sample_loads.size(), 1u);
    EXPECT_EQ(GetDispatchRecord().sample_loads[0].sample_id, 7);
    EXPECT_STREQ(GetDispatchRecord().sample_loads[0].path, "/SAMPLES/kick.wav");
}

TEST_F(MessageDispatchTest, StatusRequestSendsSampleMemStatusResponse) {
    StatusRequestMessage req(STATUS_CATEGORY_SAMPLE_MEM);
    Dispatch(MSG_STATUS_REQUEST, req);

    EXPECT_EQ(GetDispatchRecord().get_sample_mem_status_calls, 1);
    ASSERT_EQ(GetDispatchRecord().uart_sends.size(), 1u);
    EXPECT_EQ(GetDispatchRecord().uart_sends[0].msg_type, MSG_STATUS_RESPONSE);
}

TEST_F(MessageDispatchTest, BrowseRequestReachesFilesystem) {
    // Live wire format (see inter_mcu.cpp inter_mcu_send_browse_req):
    // [start_index u8][path bytes][NUL].
    const char* path = "/SAMPLES";
    uint8_t payload[1 + 9];
    payload[0] = 3;  // start_index
    std::memcpy(payload + 1, path, 9);
    ProcessInterMcuMessage(MSG_BROWSE_REQ, 1, payload, sizeof(payload));

    ASSERT_EQ(GetDispatchRecord().browse_requests.size(), 1u);
    EXPECT_EQ(GetDispatchRecord().browse_requests[0].path, "/SAMPLES");
    EXPECT_EQ(GetDispatchRecord().browse_requests[0].start_index, 3u);
}

// The path field of a CRC-valid frame carries NO guarantee of an in-payload
// NUL terminator; the handler must bound its scan to the payload instead of
// strlen()ing into whatever follows. A path that fills the payload exactly
// (no NUL anywhere) must come through truncated to the payload's bytes.
TEST_F(MessageDispatchTest, BrowsePathWithoutTerminatorIsBoundedToPayload) {
    uint8_t payload[1 + 4];
    payload[0] = 0;                       // start_index
    std::memcpy(payload + 1, "ABCD", 4);  // 4 bytes, deliberately no NUL
    ProcessInterMcuMessage(MSG_BROWSE_REQ, 1, payload, sizeof(payload));

    ASSERT_EQ(GetDispatchRecord().browse_requests.size(), 1u);
    EXPECT_EQ(GetDispatchRecord().browse_requests[0].path, "ABCD");
}

// The handler's stack buffer holds 95 characters + NUL; a longer wire path
// must be truncated there, not overflow it.
TEST_F(MessageDispatchTest, OverlongBrowsePathIsTruncatedTo95Chars) {
    std::string long_path(120, 'x');
    std::vector<uint8_t> payload(1 + long_path.size() + 1);
    payload[0] = 0;
    std::memcpy(payload.data() + 1, long_path.c_str(), long_path.size() + 1);
    ProcessInterMcuMessage(MSG_BROWSE_REQ, 1, payload.data(), static_cast<size_t>(payload.size()));

    ASSERT_EQ(GetDispatchRecord().browse_requests.size(), 1u);
    EXPECT_EQ(GetDispatchRecord().browse_requests[0].path, std::string(95, 'x'));
}

TEST_F(MessageDispatchTest, SamplePlayRequestForwardsThePath) {
    const char path[] = "/SAMPLES/kick.wav";
    ProcessInterMcuMessage(MSG_SAMPLE_PLAY_REQ,
                           1,
                           reinterpret_cast<const uint8_t*>(path),
                           sizeof(path));  // includes the NUL

    ASSERT_EQ(GetDispatchRecord().play_requests.size(), 1u);
    EXPECT_EQ(GetDispatchRecord().play_requests[0], "/SAMPLES/kick.wav");
}

// Same contract as the browse path above: a play-request path that fills the
// payload with no NUL anywhere must be bounded to the payload's bytes, and an
// overlong one truncated to the handler's 95-char buffer - never strlen()ed
// past the frame.
TEST_F(MessageDispatchTest, SamplePlayPathWithoutTerminatorIsBoundedToPayload) {
    const uint8_t payload[4] = {'A', 'B', 'C', 'D'};  // deliberately no NUL
    ProcessInterMcuMessage(MSG_SAMPLE_PLAY_REQ, 1, payload, sizeof(payload));

    ASSERT_EQ(GetDispatchRecord().play_requests.size(), 1u);
    EXPECT_EQ(GetDispatchRecord().play_requests[0], "ABCD");
}

TEST_F(MessageDispatchTest, OverlongSamplePlayPathIsTruncatedTo95Chars) {
    std::string long_path(120, 'y');
    ProcessInterMcuMessage(MSG_SAMPLE_PLAY_REQ,
                           1,
                           reinterpret_cast<const uint8_t*>(long_path.c_str()),
                           long_path.size());  // no NUL within payload_size

    ASSERT_EQ(GetDispatchRecord().play_requests.size(), 1u);
    EXPECT_EQ(GetDispatchRecord().play_requests[0], std::string(95, 'y'));
}

// A message type nothing routes must fall through the switch without
// touching any subsystem - the record must stay completely empty.
TEST_F(MessageDispatchTest, UnknownMessageTypeReachesNothing) {
    uint8_t payload[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    ProcessInterMcuMessage(0xEE, 1, payload, sizeof(payload));

    const DispatchRecord& r = GetDispatchRecord();
    EXPECT_TRUE(r.note_ons.empty());
    EXPECT_TRUE(r.note_offs.empty());
    EXPECT_TRUE(r.control_changes.empty());
    EXPECT_TRUE(r.sample_ctrls.empty());
    EXPECT_TRUE(r.sample_loads.empty());
    EXPECT_TRUE(r.browse_requests.empty());
    EXPECT_TRUE(r.play_requests.empty());
    EXPECT_TRUE(r.uart_sends.empty());
    EXPECT_TRUE(r.seq_transports.empty());
    EXPECT_TRUE(r.midi_ccs.empty());
}

TEST_F(MessageDispatchTest, SamplePlayIndexReachesFilesystem) {
    SamplePlayIndexMessage msg(42);
    Dispatch(MSG_SAMPLE_PLAY_INDEX_REQ, msg);

    ASSERT_EQ(GetDispatchRecord().play_index_requests.size(), 1u);
    EXPECT_EQ(GetDispatchRecord().play_index_requests[0], 42u);
}

TEST_F(MessageDispatchTest, SampleStopReachesFilesystem) {
    SampleStopReqMessage msg(0);
    Dispatch(MSG_SAMPLE_STOP_REQ, msg);

    ASSERT_EQ(GetDispatchRecord().stop_requests.size(), 1u);
}

TEST_F(MessageDispatchTest, CvCalSetReachesAudioEngine) {
    CvCalMessage cal(2, 1, 1.1f, -0.05f, 0.9f, 0.02f, 1.05f, -0.01f, 2.7f);
    Dispatch(MSG_CV_CAL_SET, cal);

    ASSERT_EQ(GetDispatchRecord().cv_cal_sets.size(), 1u);
    EXPECT_EQ(GetDispatchRecord().cv_cal_sets[0].group, 2);
    EXPECT_EQ(GetDispatchRecord().cv_cal_sets[0].persist, 1);
    EXPECT_FLOAT_EQ(GetDispatchRecord().cv_cal_sets[0].cutoff_k, 2.7f);
}

TEST_F(MessageDispatchTest, CvCalGetReachesAudioEngine) {
    CvCalGetMessage get(4);
    Dispatch(MSG_CV_CAL_GET, get);
    ASSERT_EQ(GetDispatchRecord().cv_cal_gets.size(), 1u);
    EXPECT_EQ(GetDispatchRecord().cv_cal_gets[0], 4);
}

TEST_F(MessageDispatchTest, CvTestReachesAudioEngine) {
    CvTestMessage test(0, 1, 0.5f, 0.1f, 1.0f);
    Dispatch(MSG_CV_TEST, test);
    ASSERT_EQ(GetDispatchRecord().cv_tests.size(), 1u);
    EXPECT_EQ(GetDispatchRecord().cv_tests[0].enable, 1);
    EXPECT_FLOAT_EQ(GetDispatchRecord().cv_tests[0].vca, 1.0f);
}

TEST_F(MessageDispatchTest, SeqTransportReachesAudioEngine) {
    SeqTransportMessage m(SEQ_TRANSPORT_PLAY, SEQ_CLOCK_MIDI, SEQ_INPUT_LIVE_RECORD, 1, 13000, 16);
    Dispatch(MSG_SEQ_TRANSPORT, m);

    ASSERT_EQ(GetDispatchRecord().seq_transports.size(), 1u);
    EXPECT_EQ(GetDispatchRecord().seq_transports[0].command, SEQ_TRANSPORT_PLAY);
    EXPECT_EQ(GetDispatchRecord().seq_transports[0].clock_source, SEQ_CLOCK_MIDI);
    EXPECT_EQ(GetDispatchRecord().seq_transports[0].tempo_bpm_x100, 13000);
}

TEST_F(MessageDispatchTest, SeqPatternOpReachesAudioEngine) {
    SeqPatternOpMessage m(SEQ_OP_SET_STEP_MICRO, 2, 9, 3, 12, -7);
    Dispatch(MSG_SEQ_PATTERN_OP, m);

    ASSERT_EQ(GetDispatchRecord().seq_pattern_ops.size(), 1u);
    EXPECT_EQ(GetDispatchRecord().seq_pattern_ops[0].op, SEQ_OP_SET_STEP_MICRO);
    EXPECT_EQ(GetDispatchRecord().seq_pattern_ops[0].track, 2);
    EXPECT_EQ(GetDispatchRecord().seq_pattern_ops[0].step, 9);
    EXPECT_EQ(GetDispatchRecord().seq_pattern_ops[0].arg_s16, -7);
}

TEST_F(MessageDispatchTest, MidiClockEventReachesAudioEngine) {
    MidiClockEventMessage m(MIDI_CLK_TICK, 1, 500, 20833, 0);
    Dispatch(MSG_MIDI_CLOCK_EVENT, m);

    ASSERT_EQ(GetDispatchRecord().midi_clock_events.size(), 1u);
    EXPECT_EQ(GetDispatchRecord().midi_clock_events[0].event, MIDI_CLK_TICK);
    EXPECT_EQ(GetDispatchRecord().midi_clock_events[0].esp_delta_us, 20833u);
}

TEST_F(MessageDispatchTest, MidiCcReachesAudioEngine) {
    MidiCcMessage m(74 /*filter cutoff CC*/, 90, 4);
    Dispatch(MSG_MIDI_CC, m);

    ASSERT_EQ(GetDispatchRecord().midi_ccs.size(), 1u);
    EXPECT_EQ(GetDispatchRecord().midi_ccs[0].cc, 74);
    EXPECT_EQ(GetDispatchRecord().midi_ccs[0].value, 90);
    EXPECT_EQ(GetDispatchRecord().midi_ccs[0].channel, 4);
}

TEST_F(MessageDispatchTest, TruncatedSeqAndMidiMessagesAreDropped) {
    uint8_t small[2] = {0, 1};
    ProcessInterMcuMessage(MSG_SEQ_TRANSPORT, 1, small, sizeof(small));
    ProcessInterMcuMessage(MSG_SEQ_PATTERN_OP, 2, small, sizeof(small));
    ProcessInterMcuMessage(MSG_MIDI_CLOCK_EVENT, 3, small, sizeof(small));
    ProcessInterMcuMessage(MSG_MIDI_CC, 4, small, 1);
    EXPECT_TRUE(GetDispatchRecord().seq_transports.empty());
    EXPECT_TRUE(GetDispatchRecord().seq_pattern_ops.empty());
    EXPECT_TRUE(GetDispatchRecord().midi_clock_events.empty());
    EXPECT_TRUE(GetDispatchRecord().midi_ccs.empty());
}

TEST_F(MessageDispatchTest, TruncatedCvMessagesAreDropped) {
    uint8_t small[2] = {0, 1};
    ProcessInterMcuMessage(MSG_CV_CAL_SET, 1, small, sizeof(small));
    ProcessInterMcuMessage(MSG_CV_CAL_GET, 2, small, 1);
    ProcessInterMcuMessage(MSG_CV_TEST, 3, small, sizeof(small));
    EXPECT_TRUE(GetDispatchRecord().cv_cal_sets.empty());
    EXPECT_TRUE(GetDispatchRecord().cv_cal_gets.empty());
    EXPECT_TRUE(GetDispatchRecord().cv_tests.empty());
}

// Truncated payloads must be rejected before touching the subsystem, for
// every newly-wired handler (review H3's bug class, Daisy side).
TEST_F(MessageDispatchTest, TruncatedPayloadsAreDroppedNotDispatched) {
    uint8_t small[2] = {60, 100};

    ProcessInterMcuMessage(MSG_NOTE_ON, 1, small, sizeof(small));
    ProcessInterMcuMessage(MSG_NOTE_OFF, 2, small, sizeof(small));
    ProcessInterMcuMessage(MSG_CONTROL_CHANGE, 3, small, sizeof(small));
    ProcessInterMcuMessage(MSG_SAMPLE_CTRL, 4, small, sizeof(small));

    EXPECT_TRUE(GetDispatchRecord().note_ons.empty());
    EXPECT_TRUE(GetDispatchRecord().note_offs.empty());
    EXPECT_TRUE(GetDispatchRecord().control_changes.empty());
    EXPECT_TRUE(GetDispatchRecord().sample_ctrls.empty());
}

TEST_F(MessageDispatchTest, NullPayloadIsSafeForAllRoutedTypes) {
    const uint8_t types[] = {MSG_NOTE_ON,
                             MSG_NOTE_OFF,
                             MSG_CONTROL_CHANGE,
                             MSG_SAMPLE_CTRL,
                             MSG_SAMPLE_LOAD,
                             MSG_SAMPLE_DATA,
                             MSG_PREVIEW_REQ,
                             MSG_STATUS_REQUEST,
                             MSG_BROWSE_REQ,
                             MSG_SAMPLE_PLAY_REQ,
                             MSG_SAMPLE_STOP_REQ,
                             MSG_SAMPLE_PLAY_INDEX_REQ};
    for (uint8_t t: types) {
        ProcessInterMcuMessage(t, 1, nullptr, 0);
    }

    const DispatchRecord& r = GetDispatchRecord();
    EXPECT_TRUE(r.note_ons.empty());
    EXPECT_TRUE(r.note_offs.empty());
    EXPECT_TRUE(r.control_changes.empty());
    EXPECT_TRUE(r.sample_ctrls.empty());
    EXPECT_TRUE(r.sample_loads.empty());
    EXPECT_TRUE(r.preview_reqs.empty());
    EXPECT_TRUE(r.browse_requests.empty());
    EXPECT_TRUE(r.play_requests.empty());
    EXPECT_TRUE(r.stop_requests.empty());
    EXPECT_TRUE(r.play_index_requests.empty());
}

// Sample selection and unload reach the engine.
//
// This is the class of bug the dispatch suite exists for: MSG_NOTE_ON once had
// a log-only stub here and nothing noticed, because every unit test around it
// tested units rather than the routing between them. A new message is not
// wired until a test says the byte on the wire reaches the engine call.
TEST_F(MessageDispatchTest, SampleSelectReachesEngine) {
    WaveX::Protocol::SampleSelectMessage msg(7);
    ProcessInterMcuMessage(
        MSG_SAMPLE_SELECT, 1, reinterpret_cast<const uint8_t*>(&msg), sizeof(msg));

    const DispatchRecord& r = GetDispatchRecord();
    ASSERT_EQ(r.selected_samples.size(), 1u);
    EXPECT_EQ(r.selected_samples[0], 7);
}

TEST_F(MessageDispatchTest, SampleSelectZeroIsForwarded) {
    // 0 is meaningful, not a no-op: it restores "most recently loaded".
    WaveX::Protocol::SampleSelectMessage msg(0);
    ProcessInterMcuMessage(
        MSG_SAMPLE_SELECT, 1, reinterpret_cast<const uint8_t*>(&msg), sizeof(msg));

    ASSERT_EQ(GetDispatchRecord().selected_samples.size(), 1u);
    EXPECT_EQ(GetDispatchRecord().selected_samples[0], 0);
}

TEST_F(MessageDispatchTest, SampleUnloadReachesEngine) {
    WaveX::Protocol::SampleUnloadMessage msg(3);
    ProcessInterMcuMessage(
        MSG_SAMPLE_UNLOAD, 1, reinterpret_cast<const uint8_t*>(&msg), sizeof(msg));

    const DispatchRecord& r = GetDispatchRecord();
    ASSERT_EQ(r.unloaded_samples.size(), 1u);
    EXPECT_EQ(r.unloaded_samples[0], 3);
}

TEST_F(MessageDispatchTest, UndersizedSampleSelectIsRejected) {
    uint8_t truncated[1] = {0};
    ProcessInterMcuMessage(MSG_SAMPLE_SELECT, 1, truncated, sizeof(truncated));
    ProcessInterMcuMessage(MSG_SAMPLE_UNLOAD, 1, truncated, sizeof(truncated));

    EXPECT_TRUE(GetDispatchRecord().selected_samples.empty());
    EXPECT_TRUE(GetDispatchRecord().unloaded_samples.empty());
}

}  // namespace
