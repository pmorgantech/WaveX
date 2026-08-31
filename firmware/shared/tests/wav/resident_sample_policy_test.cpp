#include "wav/resident_sample_policy.hpp"

#include <gtest/gtest.h>

using WaveX::Wav::IsResidentSampleFormatSupported;

TEST(ResidentSamplePolicyTest, AcceptsOnlyFormatsTheRamVoiceCanRender) {
    EXPECT_TRUE(IsResidentSampleFormatSupported(16, 1));
    EXPECT_TRUE(IsResidentSampleFormatSupported(16, 2));
    EXPECT_FALSE(IsResidentSampleFormatSupported(8, 1));
    EXPECT_FALSE(IsResidentSampleFormatSupported(24, 1));
    EXPECT_FALSE(IsResidentSampleFormatSupported(16, 0));
    EXPECT_FALSE(IsResidentSampleFormatSupported(16, 3));
}
