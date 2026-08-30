// Host tests for the SDRAM sample-memory manager (src/memory.h - HAL-free,
// header-only: slab pool for small allocations, extent pool for large).
// Added with the review-H1 bitmap resize (SlabPage::bm shrank 32x); these
// tests pin the slot bookkeeping the bitmap drives - full-page allocation
// for the smallest class, address distinctness, release/reuse - so a sizing
// regression corrupts a test, not sample RAM.

#include <gtest/gtest.h>

#include "memory.h"

#include <cstdint>
#include <set>

namespace {

constexpr uint32_t kArenaBytes = 2 * 1024 * 1024;  // 2 MiB test arena
constexpr uint32_t kSmallBytes = 256 * 1024;       // 256 KiB small pool
alignas(64) static uint8_t g_arena[kArenaBytes];

class SampleMemTest : public ::testing::Test {
   protected:
    void SetUp() override { ASSERT_TRUE(mgr_.init(g_arena, kArenaBytes, kSmallBytes)); }

    SampleMemMgr mgr_;
};

TEST_F(SampleMemTest, SmallAllocReturnsUsablePointer) {
    wxsamp_t h{};
    ASSERT_TRUE(mgr_.alloc(200, &h));
    EXPECT_NE(h.cls, 0xFF);  // routed to the slab pool

    void* p = nullptr;
    ASSERT_TRUE(mgr_.ptr(h, &p));
    ASSERT_NE(p, nullptr);
    EXPECT_GE(p, static_cast<void*>(g_arena));
    EXPECT_LT(p, static_cast<void*>(g_arena + kSmallBytes));

    // The slot must be writable across its full logical length.
    memset(p, 0xA5, 200);
    mgr_.release(&h);
}

TEST_F(SampleMemTest, LargeAllocRoutesToExtentPool) {
    wxsamp_t h{};
    ASSERT_TRUE(mgr_.alloc(200 * 1024, &h));
    EXPECT_EQ(h.cls, 0xFF);

    void* p = nullptr;
    ASSERT_TRUE(mgr_.ptr(h, &p));
    ASSERT_NE(p, nullptr);
    EXPECT_GE(p, static_cast<void*>(g_arena + kSmallBytes));
    memset(p, 0x5A, 200 * 1024);
    mgr_.release(&h);
}

// Fills complete pages of the smallest class (32 B -> 128 slots/page, the
// bitmap's worst case) and checks every slot is distinct and in-bounds.
// With a too-small bitmap this aliases slots or walks out of the page.
TEST_F(SampleMemTest, SmallestClassFillsPagesWithDistinctSlots) {
    constexpr int kAllocs = 300;  // > 2 full 128-slot pages
    wxsamp_t handles[kAllocs];
    std::set<void*> addresses;

    for (int i = 0; i < kAllocs; ++i) {
        handles[i] = wxsamp_t{};
        ASSERT_TRUE(mgr_.alloc(16, &handles[i])) << "alloc " << i;
        ASSERT_NE(handles[i].cls, 0xFF);
        void* p = nullptr;
        ASSERT_TRUE(mgr_.ptr(handles[i], &p));
        ASSERT_NE(p, nullptr);
        EXPECT_LT(p, static_cast<void*>(g_arena + kSmallBytes));
        EXPECT_TRUE(addresses.insert(p).second) << "slot " << i << " aliases another slot";
    }

    for (int i = 0; i < kAllocs; ++i) {
        mgr_.release(&handles[i]);
    }

    // After releasing everything, the same capacity must be allocatable
    // again (bitmap correctly returned every slot).
    for (int i = 0; i < kAllocs; ++i) {
        wxsamp_t h{};
        ASSERT_TRUE(mgr_.alloc(16, &h)) << "re-alloc " << i;
    }
}

TEST_F(SampleMemTest, ReleaseZeroesHandleAndSecondReleaseIsSafe) {
    wxsamp_t h{};
    ASSERT_TRUE(mgr_.alloc(64, &h));
    mgr_.release(&h);
    EXPECT_EQ(h.len, 0u);
    mgr_.release(&h);  // no-op on a zeroed handle; must not corrupt state

    wxsamp_t h2{};
    ASSERT_TRUE(mgr_.alloc(64, &h2));
    mgr_.release(&h2);
}

TEST_F(SampleMemTest, LargePoolCoalescesOnRelease) {
    // Carve the large pool into pieces, free them all, then ask for nearly
    // the whole pool - only possible if freed runs coalesced.
    wxsamp_t a{}, b{}, c{};
    ASSERT_TRUE(mgr_.alloc(400 * 1024, &a));
    ASSERT_TRUE(mgr_.alloc(400 * 1024, &b));
    ASSERT_TRUE(mgr_.alloc(400 * 1024, &c));
    mgr_.release(&b);
    mgr_.release(&a);
    mgr_.release(&c);

    wxsamp_t big{};
    ASSERT_TRUE(mgr_.alloc(1500 * 1024, &big));
    mgr_.release(&big);
}

TEST_F(SampleMemTest, StatsReflectSmallPoolActivity) {
    wxsamp_stats_t before{};
    mgr_.stats(&before);
    EXPECT_EQ(before.small_total_bytes, kSmallBytes);
    EXPECT_EQ(before.small_free_bytes, kSmallBytes);

    wxsamp_t h{};
    ASSERT_TRUE(mgr_.alloc(500, &h));

    wxsamp_stats_t during{};
    mgr_.stats(&during);
    EXPECT_GT(during.in_use_bytes, before.in_use_bytes);
    EXPECT_GE(during.objects_alive, 1u);
    EXPECT_EQ(during.small_total_bytes, kSmallBytes);
    EXPECT_LT(during.small_free_bytes, kSmallBytes);

    mgr_.release(&h);
}

}  // namespace

TEST_F(SampleMemTest, StatsCoverLargePoolToo) {
    // Review M8: in_use_bytes/objects_alive/failed_allocs used to reflect
    // only the small slab pool, leaving the UI blind to sample allocations.
    wxsamp_stats_t before{};
    mgr_.stats(&before);

    wxsamp_t big{};
    ASSERT_TRUE(mgr_.alloc(200 * 1024, &big));
    ASSERT_EQ(big.cls, 0xFF);

    wxsamp_stats_t during{};
    mgr_.stats(&during);
    EXPECT_GE(during.in_use_bytes, before.in_use_bytes + 200 * 1024);
    EXPECT_EQ(during.objects_alive, before.objects_alive + 1);

    // Impossible request: must fail AND be counted.
    wxsamp_t huge{};
    EXPECT_FALSE(mgr_.alloc(kArenaBytes * 2, &huge));
    wxsamp_stats_t after_fail{};
    mgr_.stats(&after_fail);
    EXPECT_EQ(after_fail.failed_allocs, during.failed_allocs + 1);

    mgr_.release(&big);
    wxsamp_stats_t after{};
    mgr_.stats(&after);
    EXPECT_EQ(after.in_use_bytes, before.in_use_bytes);
    EXPECT_EQ(after.objects_alive, before.objects_alive);
}

TEST_F(SampleMemTest, ZeroByteAllocRejected) {
    wxsamp_t h{};
    EXPECT_FALSE(mgr_.alloc(0, &h));
}

// release() zeroes the WHOLE handle, not just len - len==0 is the documented
// "released" sentinel a caller must check before ptr().
//
// CAUTION (production footgun, src/memory.h SampleMemMgr::ptr): ptr() itself
// does NOT check len==0 - called on a zeroed handle it routes to small class
// 0 / page 0 / slot 0 and "succeeds" with a pointer into the small pool.
// Deliberately not pinned here (it depends on pool internals); callers must
// gate on h.len, which this test keeps honest.
TEST_F(SampleMemTest, ReleaseZeroesEveryHandleField) {
    wxsamp_t h{};
    ASSERT_TRUE(mgr_.alloc(128, &h));
    mgr_.release(&h);

    EXPECT_EQ(h.len, 0u);
    EXPECT_EQ(h.cls, 0);
    EXPECT_EQ(h.page, 0);
    EXPECT_EQ(h.slot, 0);
}

// The sample browser allocates a fresh sample_id per audition, so OnSampleLoad
// never replaces an earlier load and instead retires the oldest registry entry
// to make room (audio_engine.cpp evict_oldest_loaded_sample). That recovery is
// only sound if releasing the oldest extent reliably reopens space for the next
// one - otherwise audition dies after a few dozen loads, which is exactly the
// regression this models: a long audition run in a arena that holds ~3 samples.
TEST_F(SampleMemTest, RepeatedLoadEvictOldestNeverExhaustsArena) {
    constexpr uint32_t kSampleBytes = 512 * 1024;  // ~3 fit in the large pool
    constexpr int kAuditions = 200;

    wxsamp_t live[4] = {};
    size_t count = 0;

    for (int i = 0; i < kAuditions; ++i) {
        wxsamp_t h{};
        while (!mgr_.alloc(kSampleBytes, &h)) {
            ASSERT_GT(count, 0u) << "arena could not fit one sample at audition " << i;
            mgr_.release(&live[0]);
            for (size_t j = 1; j < count; ++j) {
                live[j - 1] = live[j];
            }
            --count;
        }

        // Every audition must land on real, writable memory - a stale handle
        // surviving eviction would show up here rather than as silent garbage.
        void* p = nullptr;
        ASSERT_TRUE(mgr_.ptr(h, &p)) << "audition " << i;
        ASSERT_NE(p, nullptr) << "audition " << i;
        memset(p, i & 0xFF, kSampleBytes);

        ASSERT_LT(count, sizeof(live) / sizeof(live[0]));
        live[count++] = h;
    }

    // Eviction must actually return the memory, not merely drop the handle.
    wxsamp_stats_t st{};
    mgr_.stats(&st);
    EXPECT_EQ(st.objects_alive, count);
}

TEST(SampleMemInitTest, RejectsUnavailableOrInvalidArena) {
    SampleMemMgr mgr;
    wxsamp_t h{};

    EXPECT_FALSE(mgr.initialized());
    EXPECT_FALSE(mgr.alloc(64, &h));
    EXPECT_FALSE(mgr.init(nullptr, kArenaBytes, kSmallBytes));
    EXPECT_FALSE(mgr.init(g_arena, kSmallBytes, kSmallBytes));
    EXPECT_FALSE(mgr.alloc(64, &h));

    wxsamp_stats_t stats{};
    mgr.stats(&stats);
    EXPECT_EQ(stats.small_total_bytes, 0u);
    EXPECT_EQ(stats.large_total_bytes, 0u);
}
