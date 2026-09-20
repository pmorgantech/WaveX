#include "audio/note_pitch_table.hpp"

#include <gtest/gtest.h>

TEST(NotePitchTable, EveryByteKeyPairMatchesTheFormerTriggerExpression) {
    WaveX::AudioEngine::NotePitchTable table;
    table.Init();
    for (int note = 0; note <= 255; ++note)
        for (int root = 0; root <= 255; ++root)
            EXPECT_FLOAT_EQ(table.Ratio(static_cast<uint8_t>(note), static_cast<uint8_t>(root)),
                            std::pow(2.f, static_cast<float>(note - root) / 12.f));
}
