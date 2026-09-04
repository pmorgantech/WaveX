#include "ui/screen_blanker.h"

#include <gtest/gtest.h>

namespace {

TEST(ScreenBlankerTest, BlanksOnceAfterFiveMinutesOfInactivity) {
    wavex_ui::ScreenBlanker blanker;
    blanker.Init(1000);

    EXPECT_FALSE(blanker.ShouldBlank(1000 + wavex_ui::ScreenBlanker::kTimeoutMs - 1));
    EXPECT_TRUE(blanker.ShouldBlank(1000 + wavex_ui::ScreenBlanker::kTimeoutMs));
    EXPECT_TRUE(blanker.blanked());
    EXPECT_FALSE(blanker.ShouldBlank(1000 + wavex_ui::ScreenBlanker::kTimeoutMs + 1));
}

TEST(ScreenBlankerTest, ActivityResetsTheTimeoutAndWakesOnlyOnce) {
    wavex_ui::ScreenBlanker blanker;
    blanker.Init(0);

    blanker.RecordActivity(wavex_ui::ScreenBlanker::kTimeoutMs - 1);
    EXPECT_FALSE(blanker.ShouldBlank(wavex_ui::ScreenBlanker::kTimeoutMs));
    EXPECT_TRUE(blanker.ShouldBlank(2 * wavex_ui::ScreenBlanker::kTimeoutMs - 1));

    EXPECT_TRUE(blanker.ShouldWake(2 * wavex_ui::ScreenBlanker::kTimeoutMs));
    EXPECT_FALSE(blanker.blanked());
    EXPECT_FALSE(blanker.ShouldWake(2 * wavex_ui::ScreenBlanker::kTimeoutMs + 1));
}

TEST(ScreenBlankerTest, TimeoutIsCorrectAcrossMillisecondCounterWrap) {
    wavex_ui::ScreenBlanker blanker;
    const uint32_t start = 0xFFFFFF00u;
    blanker.Init(start);

    EXPECT_FALSE(blanker.ShouldBlank(start + wavex_ui::ScreenBlanker::kTimeoutMs - 1));
    EXPECT_TRUE(blanker.ShouldBlank(start + wavex_ui::ScreenBlanker::kTimeoutMs));
}

}  // namespace
