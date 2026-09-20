#include "profiling/callback_peak.hpp"

#include <gtest/gtest.h>

using WaveX::Profiling::CallbackPeakWindow;
using WaveX::Profiling::CallbackStage;

TEST(CallbackPeak, KeepsOneCompleteCallbackRatherThanIndependentStageMaxima) {
    CallbackPeakWindow window;
    window.Begin();
    window.Add(CallbackStage::SeqTrigger, 0, 80);
    window.Add(CallbackStage::Render, 80, 90);
    EXPECT_FALSE(window.End(0, 100));
    window.Begin();
    window.Add(CallbackStage::SeqTrigger, 0, 20);
    window.Add(CallbackStage::SeqTrigger, 20, 40);
    window.Add(CallbackStage::Render, 40, 140);
    EXPECT_FALSE(window.End(0, 150));
    const auto& peak = window.Peak();
    EXPECT_EQ(peak.block, 2u);
    EXPECT_EQ(peak.total, 150u);
    EXPECT_EQ(peak.cycles[static_cast<size_t>(CallbackStage::SeqTrigger)], 40u);
    EXPECT_EQ(peak.calls[static_cast<size_t>(CallbackStage::SeqTrigger)], 2u);
    EXPECT_EQ(peak.cycles[static_cast<size_t>(CallbackStage::Render)], 100u);
}

TEST(CallbackPeak, HandlesCycleWrapAndDropsOldPeakAtWindowBoundary) {
    CallbackPeakWindow window;
    window.Begin();
    window.Add(CallbackStage::Queue, UINT32_MAX - 9, 10);
    EXPECT_FALSE(window.End(UINT32_MAX - 19, 20));
    EXPECT_EQ(window.Peak().total, 40u);
    EXPECT_EQ(window.Peak().cycles[static_cast<size_t>(CallbackStage::Queue)], 20u);
    for (uint32_t i = 1; i < CallbackPeakWindow::kWindowBlocks; ++i) {
        window.Begin();
        EXPECT_EQ(window.End(100, 101), i == CallbackPeakWindow::kWindowBlocks - 1);
    }
    EXPECT_EQ(window.Peak().block, 1u);
    EXPECT_EQ(window.Peak().window, 1u);
    window.Begin();
    EXPECT_FALSE(window.End(0, 2));
    EXPECT_EQ(window.Peak().window, 2u);
    EXPECT_EQ(window.Peak().block, 5001u);
    EXPECT_EQ(window.Peak().total, 2u);
    EXPECT_EQ(window.Peak().cycles[static_cast<size_t>(CallbackStage::Queue)], 0u);
}
