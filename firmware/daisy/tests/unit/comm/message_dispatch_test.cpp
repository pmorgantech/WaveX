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

}  // namespace
