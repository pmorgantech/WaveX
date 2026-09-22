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
    ASSERT_TRUE(state.completeLoad(42));
    auto second = first;
    std::strcpy(second.path, "/second.wav");
    second.sample_rate = 44100;
    state.stageLoad(2, second);
    EXPECT_EQ(state.last_load_sample_id, 42);
    EXPECT_EQ(state.last_load_sample_path, "/first.wav");
    state.cancelLoad();
    EXPECT_FALSE(state.completeLoad(2));
    EXPECT_EQ(state.last_load_sample_rate, 48000u);
    EXPECT_EQ(state.last_load_sample_id, 42);
}
TEST(SampleBrowserState, CompletionAdoptsResidentIdRatherThanRequestTag) {
    wavex_ui::SampleBrowserState state;
    wavex_file_entry_t file{};
    std::strcpy(file.path, "/shared.wav");
    file.channels = 2;
    state.stageLoad(3, file);
    ASSERT_TRUE(state.completeLoad(91));
    EXPECT_EQ(state.last_load_sample_id, 91);
    EXPECT_EQ(state.last_load_sample_path, "/shared.wav");
    EXPECT_EQ(state.last_load_channels, 2);
    EXPECT_FALSE(state.loading());
    EXPECT_FALSE(state.completeLoad(92));
}
