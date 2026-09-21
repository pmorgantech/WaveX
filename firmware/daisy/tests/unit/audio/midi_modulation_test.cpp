#include "audio/midi_modulation.hpp"

#include <gtest/gtest.h>
using namespace WaveX::AudioEngine;
TEST(MidiModulation, CoalescesTracksAndSourcesWithoutMutatingConsumerSnapshot) {
    MidiModulation state;
    state.Init();
    const auto& before = state.Acquire();
    state.Wheel(0x8001, 127);
    state.Pressure(0x0002, 64);
    state.Wheel(0x0001, 32);
    EXPECT_FLOAT_EQ(before[0].wheel, 0);
    const auto& after = state.Acquire();
    EXPECT_FLOAT_EQ(after[0].wheel, 32.f / 127);
    EXPECT_FLOAT_EQ(after[15].wheel, 1);
    EXPECT_FLOAT_EQ(after[1].pressure, 64.f / 127);
    EXPECT_FLOAT_EQ(after[1].wheel, 0);
    state.Pressure(0x8000, 127);
    state.Reset(0x0001);
    const auto& reset = state.Acquire();
    EXPECT_FLOAT_EQ(reset[0].wheel, 0);
    EXPECT_FLOAT_EQ(reset[15].wheel, 1);
    EXPECT_FLOAT_EQ(reset[15].pressure, 1);
    EXPECT_FLOAT_EQ(reset[1].pressure, 64.f / 127);
    state.Reset(0xffff);
    for (const auto& v: state.Acquire()) {
        EXPECT_FLOAT_EQ(v.wheel, 0);
        EXPECT_FLOAT_EQ(v.pressure, 0);
    }
}
TEST(MidiModulation, RejectsInvalidValuesAndEmptyRoutingMask) {
    MidiModulation state;
    state.Init();
    state.Wheel(0xffff, 128);
    state.Pressure(0xffff, 255);
    state.Wheel(0, 127);
    for (const auto& v: state.Acquire()) {
        EXPECT_FLOAT_EQ(v.wheel, 0);
        EXPECT_FLOAT_EQ(v.pressure, 0);
    }
}
