#include "sequencer/pattern_exchange.hpp"

#include <gtest/gtest.h>
using namespace WaveX::Sequencer;
using namespace WaveX::Protocol;
TEST(PatternExchangeTest, RevisionChangeRestartsCaptureBeforePublishing) {
    SequencerTransport transport;
    transport.Init(48000, 48);
    PatternExchange exchange;
    ASSERT_TRUE(exchange.Capture());
    for (int i = 0; i < 10; ++i)
        exchange.Process(transport);
    EXPECT_EQ(exchange.state(), PatternExchange::State::Capture);
    transport.ApplyPatternOp({SEQ_OP_PATTERN_SWING, 0, 0, 75, 0, 0});
    transport.ApplyPatternOp({SEQ_OP_SET_STEP_NOTE, 0, 0, 75, 0, 0});
    for (int i = 0; i < 15; ++i)
        exchange.Process(transport);
    EXPECT_EQ(exchange.state(), PatternExchange::State::Capture);
    exchange.Process(transport);
    ASSERT_EQ(exchange.state(), PatternExchange::State::Captured);
    EXPECT_EQ(exchange.foreground().swing, 75);
    EXPECT_EQ(exchange.foreground().tracks[0].steps[0].note, 75);
    transport.ApplyPatternOp({SEQ_OP_SET_STEP_NOTE, 0, 0, 90, 0, 0});
    EXPECT_EQ(exchange.foreground().tracks[0].steps[0].note, 75);
    exchange.Retire();
    EXPECT_EQ(exchange.state(), PatternExchange::State::Idle);
}
TEST(PatternExchangeTest, CaptureTimeoutReturnsOwnershipWithoutBlockingAudio) {
    SequencerTransport transport;
    transport.Init(48000, 48);
    PatternExchange exchange;
    exchange.Capture();
    for (int i = 0; i < 501; ++i) {
        transport.ApplyPatternOp({SEQ_OP_PATTERN_SWING, 0, 0, 50, 0, 0});
        exchange.Process(transport);
    }
    EXPECT_EQ(exchange.state(), PatternExchange::State::Failed);
    exchange.Retire();
    EXPECT_TRUE(exchange.Capture());
}
TEST(PatternExchangeTest, InstallStopsWithoutChangingTempoAndRequiresOwnershipReturn) {
    SequencerTransport transport;
    transport.Init(48000, 48);
    transport.ApplyTransport({SEQ_TRANSPORT_PLAY, SEQ_CLOCK_INTERNAL, SEQ_INPUT_PLAY, 0, 14300, 0});
    ASSERT_TRUE(transport.IsPlaying());
    PatternExchange exchange;
    exchange.foreground().length = 64;
    exchange.foreground().tracks[15].steps[63].note = 127;
    exchange.foreground().tracks[15].steps[63].on = true;
    ASSERT_TRUE(exchange.Install());
    EXPECT_FALSE(exchange.Capture());
    exchange.Retire();  // cannot cancel callback ownership
    EXPECT_EQ(exchange.state(), PatternExchange::State::Install);
    exchange.Process(transport);
    EXPECT_EQ(exchange.state(), PatternExchange::State::Installed);
    EXPECT_FALSE(transport.IsPlaying());
    EXPECT_FALSE(transport.IsArmed());
    EXPECT_DOUBLE_EQ(transport.TempoBpm(), 143.0);
    TriggerEvent events[128];
    EXPECT_EQ(transport.Tick(events, 128), 0u);
    EXPECT_EQ(transport.pattern().length, 64);
    EXPECT_EQ(transport.pattern().tracks[15].steps[63].note, 127);
    exchange.Retire();
    EXPECT_TRUE(exchange.Capture());
}

TEST(PatternExchangeTest, CaptureDefersRowsOnTriggerBlocksButInstallDoesNotWait) {
    SequencerTransport transport;
    transport.Init(48000, 48);
    PatternExchange exchange;
    ASSERT_TRUE(exchange.Capture());
    for (int i = 0; i < 16; ++i)
        EXPECT_FALSE(exchange.Process(transport, false));
    EXPECT_EQ(exchange.state(), PatternExchange::State::Capture);
    for (int i = 0; i < 16; ++i)
        EXPECT_FALSE(exchange.Process(transport, true));
    EXPECT_EQ(exchange.state(), PatternExchange::State::Captured);
    exchange.Retire();
    ASSERT_TRUE(exchange.Install());
    EXPECT_TRUE(exchange.Process(transport, false));
    EXPECT_EQ(exchange.state(), PatternExchange::State::Installed);
}

TEST(PatternExchangeTest, ProjectPausePreservesSettingsAndSessionInstallRestoresThem) {
    SequencerTransport transport;
    transport.Init(48000, 48);
    transport.ApplyTransport(
        {SEQ_TRANSPORT_PLAY, SEQ_CLOCK_MIDI, SEQ_INPUT_STEP_RECORD, 1, 15725, 0});
    PatternExchange exchange;
    ASSERT_TRUE(exchange.Pause());
    EXPECT_TRUE(exchange.Process(transport));
    EXPECT_FALSE(transport.IsArmed());
    EXPECT_FALSE(transport.IsPlaying());
    EXPECT_DOUBLE_EQ(transport.TempoBpm(), 157.25);
    exchange.Retire();
    ASSERT_TRUE(exchange.Capture());
    for (int i = 0; i < 16; ++i)
        exchange.Process(transport);
    const auto saved = exchange.capturedSettings();
    EXPECT_EQ(saved.tempo_bpm_x100, 15725);
    EXPECT_EQ(saved.clock_source, SEQ_CLOCK_MIDI);
    EXPECT_EQ(saved.input_mode, SEQ_INPUT_STEP_RECORD);
    EXPECT_EQ(saved.quantize, 1);
    exchange.Retire();
    transport.ApplyTransport({SEQ_TRANSPORT_PLAY, SEQ_CLOCK_INTERNAL, SEQ_INPUT_PLAY, 0, 10000, 0});
    exchange.foreground().length = 31;
    ASSERT_TRUE(exchange.InstallSession(saved));
    EXPECT_TRUE(exchange.Process(transport));
    EXPECT_DOUBLE_EQ(transport.TempoBpm(), 157.25);
    EXPECT_TRUE(transport.UsingMidiSync());
    EXPECT_EQ(transport.InputMode(), SEQ_INPUT_STEP_RECORD);
    EXPECT_FALSE(transport.IsPlaying());
    EXPECT_FALSE(transport.IsArmed());
}

TEST(PatternExchangeTest, StoppedCaptureRefusesPlayingArmedAndMidCaptureStarts) {
    SequencerTransport transport;
    transport.Init(48000, 48);
    PatternExchange exchange;
    for (uint8_t clock: {SEQ_CLOCK_INTERNAL, SEQ_CLOCK_MIDI}) {
        transport.ApplyTransport({SEQ_TRANSPORT_PLAY, clock, SEQ_INPUT_PLAY, 0, 12000, 0});
        ASSERT_TRUE(exchange.Capture(true));
        exchange.Process(transport);
        EXPECT_EQ(exchange.state(), PatternExchange::State::Running);
        EXPECT_TRUE(transport.IsPlaying() || transport.IsArmed());
        exchange.Retire();
    }
    transport.StopForProject();
    ASSERT_TRUE(exchange.Capture(true));
    for (int i = 0; i < 15; ++i)
        exchange.Process(transport);
    transport.ApplyTransport({SEQ_TRANSPORT_PLAY, SEQ_CLOCK_INTERNAL, SEQ_INPUT_PLAY, 0, 12000, 0});
    exchange.Process(transport);
    EXPECT_EQ(exchange.state(), PatternExchange::State::Running);
    EXPECT_TRUE(transport.IsPlaying());
}

TEST(PatternExchangeTest, QueuedLaunchReturnsLatestOutgoingEditsAtExactGridBoundary) {
    SequencerTransport transport;
    transport.Init(48000, 48);
    transport.pattern().length = 1;
    transport.pattern().tracks[0].steps[0].on = true;
    transport.pattern().tracks[0].steps[0].note = 60;
    transport.ApplyTransport({SEQ_TRANSPORT_PLAY, SEQ_CLOCK_INTERNAL, SEQ_INPUT_PLAY, 0, 12300, 0});
    PatternExchange exchange;
    exchange.foreground().length = 3;
    exchange.foreground().scale = StepScale::Eighth;
    exchange.foreground().tracks[0].steps[0].on = true;
    exchange.foreground().tracks[0].steps[0].note = 72;
    ASSERT_TRUE(exchange.Launch(127));
    TriggerEvent events[32];
    transport.Tick(events, 32);
    exchange.Process(transport);
    ASSERT_EQ(exchange.state(), PatternExchange::State::Launching);
    const auto boundary = transport.scheduler().QueuedBoundaryFrame();
    EXPECT_EQ(boundary, 5854u);  // non-block-aligned sixteenth at 123 BPM
    transport.ApplyPatternOp({SEQ_OP_SET_STEP_NOTE, 15, 63, 117, 0, 0});
    bool destination = false;
    for (int i = 0; i < 200 && exchange.state() == PatternExchange::State::Launching; ++i) {
        const auto n = transport.Tick(events, 32);
        for (size_t j = 0; j < n; ++j) {
            if (events[j].note == 72) {
                destination = true;
                EXPECT_EQ(events[j].frame, boundary);
            }
            EXPECT_FALSE(events[j].note == 60 && events[j].frame >= boundary);
        }
        exchange.Process(transport);
    }
    ASSERT_TRUE(destination);
    ASSERT_EQ(exchange.state(), PatternExchange::State::Launched);
    EXPECT_EQ(exchange.foreground().tracks[15].steps[63].note, 117);
    EXPECT_EQ(transport.pattern().length, 3);
    EXPECT_EQ(transport.BuildPlayhead().pattern, 127);
    EXPECT_TRUE(transport.IsPlaying());
    exchange.Retire();
    ASSERT_TRUE(exchange.Capture());
    for (int i = 0; i < 16; ++i)
        exchange.Process(transport);
    EXPECT_EQ(exchange.foreground().tracks[0].steps[0].note, 72);
}
TEST(PatternExchangeTest, StopCancelsQueuedBufferWithoutInstallingItAndStoppedLaunchSwaps) {
    SequencerTransport transport;
    transport.Init(48000, 48);
    transport.ApplyTransport({SEQ_TRANSPORT_PLAY, SEQ_CLOCK_INTERNAL, SEQ_INPUT_PLAY, 0, 12000, 0});
    PatternExchange exchange;
    exchange.foreground().tracks[0].steps[0].note = 99;
    ASSERT_TRUE(exchange.Launch(4));
    exchange.Process(transport);
    transport.StopForProject();
    TriggerEvent events[32];
    transport.Tick(events, 32);
    exchange.Process(transport);
    EXPECT_EQ(exchange.state(), PatternExchange::State::Cancelled);
    EXPECT_EQ(transport.pattern().tracks[0].steps[0].note, 60);
    exchange.Retire();
    ASSERT_TRUE(exchange.Launch(4));
    exchange.Process(transport);
    EXPECT_EQ(exchange.state(), PatternExchange::State::Launched);
    EXPECT_EQ(transport.pattern().tracks[0].steps[0].note, 99);
    EXPECT_EQ(exchange.foreground().tracks[0].steps[0].note, 60);
    EXPECT_FALSE(transport.IsPlaying());
}

TEST(PatternExchangeTest, ScopedEditsRejectOutgoingEpochIncludingReturnToSameSlot) {
    SequencerTransport transport;
    transport.Init(48000, 48);
    SeqPatternRequestMessage request;
    request.request_id = 1;
    SeqSlotPageMessage before;
    transport.BuildSlotPage(request, before);
    SeqSlotEditMessage edit;
    edit.pattern = before.pattern;
    edit.epoch = before.epoch;
    edit.edit = {SEQ_OP_SET_STEP_NOTE, 0, 0, 99, 0, 0};
    EXPECT_TRUE(transport.ApplySlotEdit(edit));
    PatternExchange exchange;
    ASSERT_TRUE(exchange.Launch(1));
    exchange.Process(transport);
    exchange.Retire();
    ASSERT_TRUE(exchange.Launch(0));
    exchange.Process(transport);
    exchange.Retire();
    EXPECT_EQ(transport.BuildPlayhead().pattern, 0);
    EXPECT_FALSE(transport.ApplySlotEdit(edit));
    SeqSlotPageMessage after;
    transport.BuildSlotPage(request, after);
    EXPECT_NE(after.epoch, before.epoch);
    edit.epoch = after.epoch;
    EXPECT_TRUE(transport.ApplySlotEdit(edit));
    transport.ReplacePattern(Pattern{});
    EXPECT_FALSE(transport.ApplySlotEdit(edit));
}
