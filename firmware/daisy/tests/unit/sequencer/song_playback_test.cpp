#include <gtest/gtest.h>

#include "sequencer/pattern_exchange.hpp"
#include <memory>
#include <vector>
using namespace WaveX::Sequencer;
using namespace WaveX::Protocol;
namespace {
class SongPlaybackTest : public ::testing::Test {
   protected:
    std::unique_ptr<Project> project = std::make_unique<Project>();
    SequencerTransport transport;
    PatternExchange exchange;
    void SetUp() override {
        transport.Init(48000, 48);
        for (uint8_t i = 0; i < 2; ++i) {
            auto& slot = project->patterns[i];
            slot.used = true;
            slot.pattern.length = 1;
            auto& step = slot.pattern.tracks[0].steps[0];
            step.on = true;
            step.note = 60 + i;
        }
        auto& song = project->songs[0];
        song.used = true;
        song.length = 2;
        song.entries[0] = {0, 2};
        song.entries[1] = {1, 3};
        song.tempo_bpm_x100 = 12300;
    }
    void Start(bool loop = false, uint8_t entry = 0) {
        ASSERT_TRUE(exchange.PlaySong(project.get(), 0, entry, loop));
        ASSERT_TRUE(exchange.Process(transport));
        ASSERT_TRUE(transport.SongActive());
    }
    void Block(std::vector<TriggerEvent>& events) {
        TriggerEvent block[32];
        auto n = transport.Tick(block, 32);
        for (size_t i = 0; i < n; ++i)
            events.push_back(block[i]);
        exchange.Process(transport);
    }
};
}  // namespace
TEST_F(SongPlaybackTest, RepeatsSwitchAndStopInsideBlocksWithoutForegroundService) {
    Start();
    std::vector<TriggerEvent> events;
    for (int i = 0; i < 1000 && transport.SongActive(); ++i)
        Block(events);
    ASSERT_EQ(events.size(), 5u);
    for (int i = 0; i < 5; ++i) {
        EXPECT_EQ(events[i].note, i < 2 ? 60 : 61);
        EXPECT_EQ(events[i].frame, static_cast<uint64_t>(i * (48000.0 * 60 / 123 / 4) + 0.5));
    }
    EXPECT_FALSE(transport.IsPlaying());
    EXPECT_EQ(exchange.state(), PatternExchange::State::SongEnded);
    EXPECT_EQ(transport.BuildPlayhead().pattern, 1);
    EXPECT_EQ(transport.pattern().tracks[0].steps[0].note, 61);
    exchange.Retire();
    EXPECT_EQ(exchange.state(), PatternExchange::State::Idle);
}
TEST_F(SongPlaybackTest, PlaybackRowsFollowSongSectionsInsteadOfWorkingPattern) {
    transport.pattern().tracks[0].enabled = false;
    project->patterns[0].pattern.tracks[0].melodic = true;
    project->patterns[1].pattern.tracks[1].melodic = true;
    Start();
    EXPECT_FALSE(transport.pattern().tracks[0].enabled);
    EXPECT_TRUE(transport.PlaybackPattern().tracks[0].enabled);
    EXPECT_TRUE(transport.PlaybackPattern().tracks[0].melodic);
    std::vector<TriggerEvent> events;
    for (int i = 0; i < 1000 && transport.BuildPlayhead().pattern == 0; ++i)
        Block(events);
    ASSERT_EQ(transport.BuildPlayhead().pattern, 1);
    EXPECT_FALSE(transport.PlaybackPattern().tracks[0].melodic);
    EXPECT_TRUE(transport.PlaybackPattern().tracks[1].melodic);
    exchange.StopSong();
    Block(events);
    EXPECT_FALSE(transport.SongActive());
    EXPECT_TRUE(transport.PlaybackPattern().tracks[1].melodic);
}
TEST_F(SongPlaybackTest, LoopSeekAndPatternEditsHaveExplicitLifetimes) {
    Start(true, 1);
    std::vector<TriggerEvent> events;
    transport.ApplyPatternOp({SEQ_OP_SET_STEP_NOTE, 0, 0, 99, 0, 0});
    for (int i = 0; i < 740; ++i)
        Block(events);
    ASSERT_GE(events.size(), 6u);
    EXPECT_EQ(events[0].note, 61);
    EXPECT_EQ(events[2].note, 61);
    EXPECT_EQ(events[3].note, 60);
    EXPECT_EQ(events[4].note, 60);
    EXPECT_EQ(events[5].note, 61);
    exchange.StopSong();
    Block(events);
    EXPECT_FALSE(transport.SongActive());
    EXPECT_FALSE(transport.IsPlaying());
    EXPECT_EQ(exchange.state(), PatternExchange::State::SongEnded);
    transport.ApplyPatternOp({SEQ_OP_SET_STEP_NOTE, 0, 0, 99, 0, 0});
    EXPECT_EQ(transport.pattern().tracks[0].steps[0].note, 99);
    EXPECT_EQ(project->patterns[1].pattern.tracks[0].steps[0].note, 61);
}
TEST_F(SongPlaybackTest, MidiStopPausesAndLocalStopReturnsOwnership) {
    transport.ApplyTransport(
        {SEQ_TRANSPORT_CONFIGURE, SEQ_CLOCK_MIDI, SEQ_INPUT_PLAY, 0, 12000, 0});
    Start();
    std::vector<TriggerEvent> events;
    for (int i = 0; i < 10; ++i)
        Block(events);
    EXPECT_TRUE(transport.IsArmed());
    EXPECT_TRUE(events.empty());
    MidiClockEventMessage clock;
    clock.event = MIDI_CLK_START;
    transport.OnMidiClock(clock);
    Block(events);
    EXPECT_TRUE(events.empty());
    clock.event = MIDI_CLK_TICK;
    clock.tick_seq = 1;
    transport.OnMidiClock(clock);
    Block(events);
    ASSERT_EQ(events.size(), 1u);
    EXPECT_EQ(events[0].frame, 0u);
    clock.event = MIDI_CLK_STOP;
    transport.OnMidiClock(clock);
    Block(events);
    EXPECT_TRUE(transport.SongActive());
    EXPECT_TRUE(transport.IsArmed());
    exchange.StopSong();
    Block(events);
    EXPECT_FALSE(transport.SongActive());
    EXPECT_FALSE(transport.IsArmed());
    EXPECT_EQ(exchange.state(), PatternExchange::State::SongEnded);
}
TEST_F(SongPlaybackTest, LastSectionOfFullArrangementAndMaximumRepeatAreBounded) {
    auto& song = project->songs[0];
    song.length = 128;
    song.tempo_bpm_x100 = 30000;
    for (uint16_t i = 0; i < song.length; ++i)
        song.entries[i] = {static_cast<uint8_t>(i % 2), 1};
    song.entries[127].repeats = 255;
    project->patterns[0].pattern.scale = project->patterns[1].pattern.scale =
        StepScale::ThirtySecond;
    Start(false, 126);
    std::vector<TriggerEvent> events;
    for (int i = 0; i < 10000 && transport.SongActive(); ++i)
        Block(events);
    ASSERT_EQ(events.size(), 256u);
    EXPECT_EQ(events.front().note, 60);
    EXPECT_EQ(events.back().note, 61);
    EXPECT_FALSE(transport.SongActive());
    EXPECT_EQ((exchange.SongPosition() >> 8) & 0x7f, 127u);
}

TEST_F(SongPlaybackTest, SppLocatesSectionAndRemainingRepeats) {
    transport.ApplyTransport({SEQ_TRANSPORT_CONFIGURE, SEQ_CLOCK_MIDI, 0, 0, 12300, 0});
    Start();
    transport.OnMidiClock({MIDI_CLK_SPP, 0, 0, 0, 3});  // second repeat of section 2
    transport.OnMidiClock({MIDI_CLK_CONTINUE, 0, 0, 0, 0});
    transport.OnMidiClock({MIDI_CLK_TICK, 0, 1, 0, 0});
    std::vector<TriggerEvent> events;
    for (int i = 0; i < 1000 && transport.SongActive(); ++i)
        Block(events);
    ASSERT_EQ(events.size(), 2u);
    EXPECT_EQ(events[0].note, 61);
    EXPECT_EQ(events[1].note, 61);
    EXPECT_FALSE(transport.SongActive());
}

TEST_F(SongPlaybackTest, SppAtEndStopsFiniteSongAndWrapsLoopingSong) {
    transport.ApplyTransport({SEQ_TRANSPORT_CONFIGURE, SEQ_CLOCK_MIDI, 0, 0, 12300, 0});
    Start(true);
    transport.OnMidiClock({MIDI_CLK_SPP, 0, 0, 0, 8});  // length 5: section 2 repeat 2
    transport.OnMidiClock({MIDI_CLK_CONTINUE, 0, 0, 0, 0});
    transport.OnMidiClock({MIDI_CLK_TICK, 0, 1, 0, 0});
    std::vector<TriggerEvent> events;
    Block(events);
    ASSERT_EQ(events.size(), 1u);
    EXPECT_EQ(events[0].note, 61);
    EXPECT_EQ((transport.SongPosition() >> 16) & 255, 2u);
    exchange.StopSong();
    Block(events);
    exchange.Retire();
    Start();
    transport.OnMidiClock({MIDI_CLK_SPP, 0, 1, 0, 5});
    transport.OnMidiClock({MIDI_CLK_CONTINUE, 0, 1, 0, 0});
    transport.OnMidiClock({MIDI_CLK_TICK, 0, 2, 0, 0});
    EXPECT_FALSE(transport.SongActive());
    EXPECT_FALSE(transport.IsPlaying());
}
