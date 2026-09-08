#include <gtest/gtest.h>

#include "sequencer/sequencer_transport.hpp"

using namespace WaveX::Protocol;
using WaveX::Sequencer::SequencerTransport;

TEST(SequencerReadback, LastTrackLastPageContainsAcceptedEditsAndLocks) {
    SequencerTransport transport;
    transport.Init(48000, 48);
    transport.ApplyPatternOp({SEQ_OP_PATTERN_LENGTH, 0, 0, 0, 64, 0});
    transport.ApplyPatternOp({SEQ_OP_PATTERN_SWING, 0, 0, 67, 0, 0});
    transport.ApplyPatternOp({SEQ_OP_SET_STEP, 15, 63, 1, 103, 0});
    transport.ApplyPatternOp({SEQ_OP_SET_STEP_PROB, 15, 63, 73, 0, 0});
    transport.ApplyPatternOp({SEQ_OP_SET_STEP_MICRO, 15, 63, 3, 7, -11});
    for (uint8_t i = 0; i < 4; ++i)
        transport.ApplyPatternOp({SEQ_OP_SET_PARAM_LOCK,
                                  15,
                                  63,
                                  static_cast<uint8_t>(i + 1),
                                  static_cast<uint16_t>(12000 + i),
                                  0});
    SeqPatternRequestMessage request;
    request.request_id = 123;
    request.track = 15;
    request.first_step = 48;
    SeqPatternSyncMessage page;
    transport.BuildPatternPage(request, page);
    EXPECT_EQ(page.request_id, 123u);
    EXPECT_EQ(page.valid, 1);
    EXPECT_EQ(page.track, 15);
    EXPECT_EQ(page.first_step, 48);
    EXPECT_EQ(page.length, 64);
    EXPECT_EQ(page.swing, 67);
    EXPECT_EQ(page.enabled, 1);
    for (uint8_t i = 0; i < 15; ++i)
        EXPECT_EQ(page.steps[i].on, 0);
    const auto& last = page.steps[15];
    EXPECT_EQ(last.on, 1);
    EXPECT_EQ(last.velocity, 103);
    EXPECT_EQ(last.probability, 73);
    EXPECT_EQ(last.retrig_count, 3);
    EXPECT_EQ(last.retrig_rate_ticks, 7);
    EXPECT_EQ(last.micro_offset, -11);
    for (uint8_t i = 0; i < 4; ++i) {
        EXPECT_EQ(last.locks[i].parameter, i + 1);
        EXPECT_EQ(last.locks[i].value, 12000 + i);
    }
}

TEST(SequencerReadback, InvalidRequestNeverIndexesPatternOrRetainsOldData) {
    SequencerTransport transport;
    transport.Init(48000, 48);
    SeqPatternRequestMessage request;
    request.request_id = 41;
    SeqPatternSyncMessage page;
    for (auto track: {uint8_t{0}, uint8_t{15}, uint8_t{16}, uint8_t{255}}) {
        for (auto first: {uint8_t{0}, uint8_t{48}, uint8_t{49}, uint8_t{64}, uint8_t{255}}) {
            request.track = track;
            request.first_step = first;
            page.valid = 1;
            page.steps[0].on = 1;
            transport.BuildPatternPage(request, page);
            EXPECT_EQ(page.request_id, 41u);
            EXPECT_EQ(page.valid, IsValidSeqPatternRequest(request) ? 1 : 0);
            EXPECT_EQ(page.steps[0].on, 0);
        }
    }
}

TEST(SequencerReadback, ConfigureChangesTempoWithoutRestartingPlayback) {
    SequencerTransport transport;
    transport.Init(48000, 48);
    transport.ApplyTransport({SEQ_TRANSPORT_PLAY, SEQ_CLOCK_INTERNAL, SEQ_INPUT_PLAY, 0, 12000, 0});
    WaveX::Sequencer::TriggerEvent events[64];
    for (int i = 0; i < 300; ++i)
        transport.Tick(events, 64);
    const auto frame = transport.scheduler().CurrentFrame();
    const auto step = transport.scheduler().PlayheadStep();
    transport.ApplyTransport(
        {SEQ_TRANSPORT_CONFIGURE, SEQ_CLOCK_INTERNAL, SEQ_INPUT_PLAY, 0, 15050, 0});
    EXPECT_TRUE(transport.IsPlaying());
    EXPECT_EQ(transport.scheduler().CurrentFrame(), frame);
    EXPECT_EQ(transport.scheduler().PlayheadStep(), step);
    EXPECT_DOUBLE_EQ(transport.TempoBpm(), 150.5);
}

TEST(SequencerReadback, ClearTrackResetsHiddenStepsAndLocksButPreservesMuteAndOtherTracks) {
    SequencerTransport transport;
    transport.Init(48000, 48);
    transport.ApplyPatternOp({SEQ_OP_TRACK_MUTE, 15, 0, 0, 0, 0});
    transport.ApplyPatternOp({SEQ_OP_SET_STEP, 15, 63, 1, 122, 0});
    transport.ApplyPatternOp({SEQ_OP_SET_PARAM_LOCK, 15, 63, 2, 45678, 0});
    transport.ApplyPatternOp({SEQ_OP_SET_STEP, 1, 63, 1, 101, 0});
    transport.ApplyPatternOp({SEQ_OP_CLEAR_TRACK, 15, 0, 0, 0, 0});
    const auto& pattern = transport.pattern();
    EXPECT_FALSE(pattern.tracks[15].enabled);
    EXPECT_FALSE(pattern.tracks[15].steps[63].on);
    EXPECT_EQ(pattern.tracks[15].steps[63].velocity, 100);
    EXPECT_EQ(pattern.tracks[15].steps[63].param_locks[0].param_id, 0);
    EXPECT_TRUE(pattern.tracks[1].steps[63].on);
}
