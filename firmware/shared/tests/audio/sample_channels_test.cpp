#include "audio/sample_channels.hpp"

#include <gtest/gtest.h>

TEST(SampleChannels, NativeAndSelectedStereoKeepPolarityAndMonoIgnoresMissingRight) {
    const int16_t expected[][2] = {{24000, -8000}, {24000, 24000}, {-8000, -8000}, {8000, 8000}};
    for (uint8_t mode = 0; mode < 4; ++mode) {
        int16_t left = 24000, right = -8000;
        WaveX::SampleChannels::Map(left, right, 2, mode);
        EXPECT_EQ(left, expected[mode][0]);
        EXPECT_EQ(right, expected[mode][1]);
        left = -32768;
        right = 32767;
        WaveX::SampleChannels::Map(left, right, 1, mode);
        EXPECT_EQ(left, -32768);
        EXPECT_EQ(right, -32768);
    }
}
TEST(SampleChannels, SumDoesNotOverflowAndOpposingChannelsCancel) {
    for (int16_t value: {int16_t(-32768), int16_t(32767)}) {
        int16_t left = value, right = value;
        WaveX::SampleChannels::Map(left, right, 2, WaveX::Protocol::SAMPLE_CH_MONO_SUM);
        EXPECT_EQ(left, value);
        EXPECT_EQ(right, value);
        left = 24000;
        right = -24000;
        WaveX::SampleChannels::Map(left, right, 2, WaveX::Protocol::SAMPLE_CH_MONO_SUM);
        EXPECT_EQ(left, 0);
        EXPECT_EQ(right, 0);
    }
}
