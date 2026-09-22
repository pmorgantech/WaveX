#include "audio/sample_gain.hpp"

#include <gtest/gtest.h>

#include <array>

using namespace WaveX::AudioEngine;
TEST(SampleGainTest, StreamingMatchesResidentGainAcrossEveryControlValue) {
    const std::array<int16_t, 9> source{-32768, -16000, -1000, -1, 0, 1, 1000, 16000, 32767};
    for (int16_t db = -240; db <= 120; ++db) {
        auto pcm = source;
        ApplySampleGain(pcm.data(), pcm.size(), SampleGainQ13(db));
        const auto gain = SampleGainLinear(db);
        for (size_t i = 0; i < pcm.size(); ++i)
            EXPECT_NEAR(pcm[i], std::clamp(source[i] * gain, -32768.f, 32767.f), 3.f)
                << "dB x10 " << db << " input " << source[i];
        if (db == 0)
            EXPECT_EQ(pcm, source);
    }
}
TEST(SampleGainTest, MinimumIsNotMuteAndPositiveGainAmplifiesWithSaturation) {
    EXPECT_NEAR(SampleGainLinear(-240), .0630957f, .000001f);
    int16_t pcm[] = {-32768, -1000, 1000, 32767};
    ApplySampleGain(pcm, 4, SampleGainQ13(120));
    EXPECT_EQ(pcm[0], -32768);
    EXPECT_NEAR(pcm[1], -3981, 1);
    EXPECT_NEAR(pcm[2], 3981, 1);
    EXPECT_EQ(pcm[3], 32767);
    EXPECT_EQ(SampleGainQ13(-32768), SampleGainQ13(-240));
    EXPECT_EQ(SampleGainQ13(32767), SampleGainQ13(120));
}
