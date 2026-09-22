#include <gtest/gtest.h>

#include "ui/ui_sample_browser.h"

#include <cstring>

TEST(SampleBrowserState, FailedLoadDoesNotReplaceConfirmedIdentityOrGeometry) {
    wavex_ui::SampleBrowserState state;
    wavex_file_entry_t first{};
    std::strcpy(first.path, "/first.wav");
    first.sample_rate = 48000;
    first.size_bytes = 96000;
    state.stageLoad(1, first);
    ASSERT_TRUE(state.completeLoad(1, 42));
    auto second = first;
    std::strcpy(second.path, "/second.wav");
    second.sample_rate = 44100;
    state.stageLoad(2, second);
    EXPECT_EQ(state.last_load_sample_id, 42);
    EXPECT_EQ(state.last_load_sample_path, "/first.wav");
    state.cancelLoad();
    EXPECT_FALSE(state.completeLoad(2, 2));
    EXPECT_EQ(state.last_load_sample_rate, 48000u);
    EXPECT_EQ(state.last_load_sample_id, 42);
}
TEST(SampleBrowserState, CompletionAdoptsResidentIdRatherThanRequestTag) {
    wavex_ui::SampleBrowserState state;
    wavex_file_entry_t file{};
    std::strcpy(file.path, "/shared.wav");
    file.channels = 2;
    state.stageLoad(3, file);
    ASSERT_TRUE(state.completeLoad(3, 91));
    EXPECT_EQ(state.last_load_sample_id, 91);
    EXPECT_EQ(state.last_load_sample_path, "/shared.wav");
    EXPECT_EQ(state.last_load_channels, 2);
    EXPECT_FALSE(state.loading());
    EXPECT_FALSE(state.completeLoad(3, 92));
}

TEST(SampleBrowserState, OldCompletionCannotAdoptNewLoadAfterExitAndReentry) {
    using wavex_ui::SampleBrowserState;
    wavex_file_entry_t file{};
    std::strcpy(file.path, "/new.wav");
    SampleBrowserState first;
    const auto old = first.allocateLoadRequestId();
    first.stageLoad(old, file);
    first.cancelLoad();
    // A recreated page/model and reset still share the process-wide sequence.
    SampleBrowserState second;
    second.reset();
    const auto current = second.allocateLoadRequestId();
    ASSERT_NE(old, current);
    second.stageLoad(current, file);
    EXPECT_FALSE(second.matchesLoad(old));  // also excludes old failure/progress
    EXPECT_FALSE(second.completeLoad(old, 42));
    EXPECT_TRUE(second.loading());
    EXPECT_EQ(second.last_load_sample_id, 0);
    EXPECT_FALSE(second.matchesLoad(0));  // legacy load statuses cannot satisfy it
    ASSERT_TRUE(second.completeLoad(current, 91));
    EXPECT_EQ(second.last_load_sample_id, 91);
    EXPECT_FALSE(second.completeLoad(current, 92));  // duplicate
    EXPECT_EQ(second.last_load_sample_id, 91);
}
