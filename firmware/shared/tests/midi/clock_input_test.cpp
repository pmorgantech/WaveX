#include "midi/clock_input.hpp"

#include <gtest/gtest.h>

#include "midi/midi_stream_parser.hpp"
using namespace WaveX;
TEST(MidiClockInput, RealtimeInterleavesWithNotesAndSppWithoutLosingState) {
    Midi::ClockInput parser(1);
    Midi::StreamParser notes;
    Midi::Event note;
    Protocol::MidiClockEventMessage clock;
    int clocks = 0, n = 0, positions = 0;
    for (uint8_t b: {0x90, 60, 0xf8, 100, 61, 0xfa, 100, 0xf2, 0x21, 0xf8, 6}) {
        if (parser.Feed(b, 1000, clock)) {
            ++clocks;
            if (clock.event == Protocol::MIDI_CLK_SPP) {
                ++positions;
                EXPECT_EQ(clock.spp_beats16, 0x321);
            }
            EXPECT_EQ(clock.source, 1);
        }
        if (notes.Feed(b, note))
            ++n;
    }
    EXPECT_EQ(n, 2);
    EXPECT_EQ(clocks, 4);
    EXPECT_EQ(positions, 1);
}
TEST(MidiClockInput, DeltaUsesAdjacentClocksNotTransportAndHandlesWrapAndReset) {
    Midi::ClockInput parser;
    Protocol::MidiClockEventMessage out;
    ASSERT_TRUE(parser.Feed(0xf8, UINT32_MAX - 100, out));
    EXPECT_EQ(out.esp_delta_us, 0u);
    ASSERT_TRUE(parser.Feed(0xfc, 10, out));
    EXPECT_EQ(out.esp_delta_us, 0u);
    ASSERT_TRUE(parser.Feed(0xf8, 19900, out));
    EXPECT_EQ(out.esp_delta_us, 20001u);
    EXPECT_EQ(out.tick_seq, 2);
    ASSERT_TRUE(parser.Feed(0xfb, 20000, out));
    ASSERT_TRUE(parser.Feed(0xf8, 21000, out));
    EXPECT_EQ(out.esp_delta_us, 0u);
    parser.Reset();
    ASSERT_TRUE(parser.Feed(0xf8, 22000, out));
    EXPECT_EQ(out.esp_delta_us, 0u);
    EXPECT_EQ(out.tick_seq, 4);
}
TEST(MidiClockInput, SystemStatusCancelsIncompletePositionAndSysexDataDoesNotLocate) {
    Midi::ClockInput parser;
    Protocol::MidiClockEventMessage out;
    for (uint8_t b: {0xf2, 3, 0x90, 60, 100, 0xf0, 2, 3, 0xf7, 1, 2})
        EXPECT_FALSE(parser.Feed(b, 0, out));
    ASSERT_FALSE(parser.Feed(0xf2, 0, out));
    ASSERT_FALSE(parser.Feed(127, 0, out));
    ASSERT_TRUE(parser.Feed(127, 0, out));
    EXPECT_EQ(out.spp_beats16, 16383);
}

TEST(MidiClockInput, BatchedClocksEstimatePeriodWithoutInventingArrivalTimes) {
    WaveX::Midi::ClockInput parser(1);
    WaveX::Protocol::MidiClockEventMessage out;
    for (uint32_t group = 0; group < 20; ++group) {
        for (unsigned clock = 0; clock < 3; ++clock) {
            ASSERT_TRUE(parser.Feed(0xf8, group * 62500, out));
            if (group && clock == 0)
                EXPECT_EQ(out.esp_delta_us, 20833u);
            else
                EXPECT_EQ(out.esp_delta_us, 0u);
        }
    }
    EXPECT_EQ(out.tick_seq, 60);
}
