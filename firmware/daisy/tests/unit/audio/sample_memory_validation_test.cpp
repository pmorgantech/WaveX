#include <gtest/gtest.h>

#include "memory.h"

#include <array>

namespace {
class SampleMemoryValidationTest : public ::testing::Test {
   protected:
    void SetUp() override { ASSERT_TRUE(memory_.init(arena_.data(), arena_.size(), 4096)); }
    std::array<uint8_t, 4096 + 4 * WXM_LARGE_PAGE_BYTES> arena_{};
    SampleMemMgr memory_;
};

TEST_F(SampleMemoryValidationTest, ReinitializationResetsSlabPlacementAndAllAccounting) {
    wxsamp_t small{}, large{}, impossible{};
    ASSERT_TRUE(memory_.alloc(32, &small));
    ASSERT_TRUE(memory_.alloc(70000, &large));
    ASSERT_FALSE(memory_.alloc(UINT32_MAX, &impossible));
    ASSERT_TRUE(memory_.init(arena_.data(), arena_.size(), 4096));
    wxsamp_stats_t stats{};
    memory_.stats(&stats);
    EXPECT_EQ(stats.objects_alive, 0u);
    EXPECT_EQ(stats.in_use_bytes, 0u);
    EXPECT_EQ(stats.failed_allocs, 0u);
    ASSERT_TRUE(memory_.alloc(32, &small));
    ASSERT_NE(small.cls, 0xff);
    void* ptr = nullptr;
    ASSERT_TRUE(memory_.ptr(small, &ptr));
    EXPECT_EQ(ptr, arena_.data());
}

TEST_F(SampleMemoryValidationTest, MalformedSlabHandlesCannotIndexOutsidePageOrBitmapTables) {
    wxsamp_t valid{};
    ASSERT_TRUE(memory_.alloc(32, &valid));
    void* ptr = nullptr;
    for (unsigned defect = 0; defect < 3; ++defect) {
        wxsamp_t malformed = valid;
        if (defect == 0)
            malformed.page = UINT16_MAX;
        if (defect == 1)
            malformed.slot = UINT16_MAX;
        if (defect == 2)
            malformed.len = 33;
        EXPECT_FALSE(memory_.ptr(malformed, &ptr));
        memory_.release(&malformed);
    }
    ASSERT_TRUE(memory_.ptr(valid, &ptr));
    wxsamp_stats_t stats{};
    memory_.stats(&stats);
    EXPECT_EQ(stats.objects_alive, 1u);
    wxsamp_t copy = valid;
    memory_.release(&valid);
    EXPECT_FALSE(memory_.ptr(copy, &ptr));
}

TEST_F(SampleMemoryValidationTest, CopiedExtentHandleCannotReleaseTheSamePagesTwice) {
    wxsamp_t original{};
    ASSERT_TRUE(memory_.alloc(70000, &original));
    wxsamp_t copy = original;
    memory_.release(&original);
    void* ptr = nullptr;
    EXPECT_FALSE(memory_.ptr(copy, &ptr));
    memory_.release(&copy);
    wxsamp_stats_t stats{};
    memory_.stats(&stats);
    EXPECT_EQ(stats.large_free_bytes, stats.large_total_bytes);
    EXPECT_EQ(stats.objects_alive, 0u);
    wxsamp_t whole{}, overlap{};
    EXPECT_TRUE(memory_.alloc(stats.large_total_bytes, &whole));
    EXPECT_FALSE(memory_.alloc(2000, &overlap));
}

TEST_F(SampleMemoryValidationTest, InvalidExtentBoundsLeaveTheLiveAllocationIntact) {
    wxsamp_t valid{};
    ASSERT_TRUE(memory_.alloc(70000, &valid));
    for (unsigned defect = 0; defect < 3; ++defect) {
        wxsamp_t malformed = valid;
        if (defect == 0)
            malformed.page = UINT16_MAX;
        if (defect == 1)
            malformed.slot = 0;
        if (defect == 2)
            malformed.len = UINT32_MAX;
        void* ptr = nullptr;
        EXPECT_FALSE(memory_.ptr(malformed, &ptr));
        memory_.release(&malformed);
    }
    wxsamp_stats_t stats{};
    memory_.stats(&stats);
    EXPECT_EQ(stats.objects_alive, 1u);
    EXPECT_EQ(stats.in_use_bytes, 2u * WXM_LARGE_PAGE_BYTES);
}
}  // namespace
