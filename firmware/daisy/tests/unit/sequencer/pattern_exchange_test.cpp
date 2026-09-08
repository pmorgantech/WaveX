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
