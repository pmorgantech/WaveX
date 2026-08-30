#include "envelope_cache.h"

#include <gtest/gtest.h>

#include <cstdlib>
#include <vector>

using WaveX::Protocol::EnvelopeChunkMessage;
using WaveX::Protocol::EnvelopeColumn;
using wavex_ui::EnvelopeCache;

namespace {

// Tracks live allocations so a test can assert the cache actually gives memory
// back on eviction - a cache that only ever grows is the failure mode that
// matters on a board with a fixed PSRAM budget.
size_t g_live_bytes = 0;
size_t g_live_blocks = 0;

void* TestAlloc(size_t bytes) {
    void* p = std::malloc(bytes + sizeof(size_t));
    if (!p) {
        return nullptr;
    }
    *static_cast<size_t*>(p) = bytes;
    g_live_bytes += bytes;
    ++g_live_blocks;
    return static_cast<char*>(p) + sizeof(size_t);
}

void TestFree(void* ptr) {
    if (!ptr) {
        return;
    }
    char* base = static_cast<char*>(ptr) - sizeof(size_t);
    g_live_bytes -= *reinterpret_cast<size_t*>(base);
    --g_live_blocks;
    std::free(base);
}

EnvelopeCache::Allocator TestAllocator() {
    EnvelopeCache::Allocator a;
    a.alloc = &TestAlloc;
    a.release = &TestFree;
    return a;
}

class EnvelopeCacheTest : public ::testing::Test {
   protected:
    void SetUp() override {
        g_live_bytes = 0;
        g_live_blocks = 0;
        cache_.init(64 * 1024, TestAllocator());
    }
    void TearDown() override {
        cache_.reset();
        EXPECT_EQ(g_live_blocks, 0u) << "cache leaked an allocation";
    }

    // Requests one run and feeds it back as a single chunk whose columns
    // ramp, so a test can tell which column it is looking at.
    bool FillRun(uint16_t sample_id,
                 uint16_t generation,
                 uint32_t view_start,
                 uint32_t view_end,
                 uint32_t total_frames,
                 uint16_t display_columns,
                 uint8_t channels = 1,
                 uint16_t max_per_request = 1280) {
        uint32_t req_start = 0, req_end = 0;
        uint16_t req_columns = 0;
        if (!cache_.nextRequest(sample_id,
                                generation,
                                view_start,
                                view_end,
                                total_frames,
                                display_columns,
                                max_per_request,
                                req_start,
                                req_end,
                                req_columns)) {
            return false;
        }
        cache_.noteRequest(sample_id, generation, req_start, req_end, req_columns);

        EnvelopeChunkMessage header;
        header.sample_id = sample_id;
        header.generation = generation;
        header.start_frame = req_start;
        header.end_frame = req_end;
        header.total_columns = req_columns;
        header.first_column = 0;
        header.columns = req_columns;
        header.channels = channels;

        std::vector<EnvelopeColumn> cols(static_cast<size_t>(req_columns) * channels);
        for (uint16_t c = 0; c < req_columns; ++c) {
            for (uint8_t ch = 0; ch < channels; ++ch) {
                cols[static_cast<size_t>(c) * channels + ch] =
                    EnvelopeColumn(static_cast<int16_t>(-(c + 1)), static_cast<int16_t>(c + 1));
            }
        }
        return cache_.ingest(header, cols.data());
    }

    EnvelopeCache cache_;
};

}  // namespace

// The tier must never be coarser than a display column, or render() would be
// stretching one measurement across several pixels and inventing detail.
TEST_F(EnvelopeCacheTest, TierIsNeverCoarserThanADisplayColumn) {
    EXPECT_EQ(EnvelopeCache::tierFramesPerColumn(1256, 1256), 1u);
    EXPECT_EQ(EnvelopeCache::tierFramesPerColumn(1256 * 4, 1256), 4u);
    // 8 M frames over 1256 columns is 6369 frames per column -> 4096, the
    // largest power of two that still fits inside one column.
    EXPECT_EQ(EnvelopeCache::tierFramesPerColumn(8000000, 1256), 4096u);
    // Zoomed past 1:1 there is nothing finer than a frame to ask for.
    EXPECT_EQ(EnvelopeCache::tierFramesPerColumn(100, 1256), 1u);
    EXPECT_EQ(EnvelopeCache::tierFramesPerColumn(0, 1256), 1u);
    EXPECT_EQ(EnvelopeCache::tierFramesPerColumn(1000, 0), 1u);
}

TEST_F(EnvelopeCacheTest, UninitialisedCacheAsksForNothing) {
    EnvelopeCache bare;
    uint32_t s = 0, e = 0;
    uint16_t c = 0;
    EXPECT_FALSE(bare.nextRequest(1, 0, 0, 1000, 1000, 256, 1280, s, e, c));
}

// The point of the cache: a view it already holds costs no round trip.
TEST_F(EnvelopeCacheTest, CoveredViewIssuesNoRequest) {
    ASSERT_TRUE(FillRun(1, 0, 0, 262144, 262144, 256));

    uint32_t s = 0, e = 0;
    uint16_t c = 0;
    EXPECT_FALSE(cache_.nextRequest(1, 0, 0, 262144, 262144, 256, 1280, s, e, c));
    EXPECT_EQ(cache_.entryCount(), 1u);
}

// A marker move does not change the audio, so the same generation must hit;
// a destructive render bumps the generation and must miss (roadmap 1.5.5 #4).
TEST_F(EnvelopeCacheTest, GenerationBumpInvalidates) {
    ASSERT_TRUE(FillRun(1, 0, 0, 262144, 262144, 256));

    uint32_t s = 0, e = 0;
    uint16_t c = 0;
    EXPECT_FALSE(cache_.nextRequest(1, 0, 0, 262144, 262144, 256, 1280, s, e, c));
    EXPECT_TRUE(cache_.nextRequest(1, 1, 0, 262144, 262144, 256, 1280, s, e, c));

    uint8_t channels = 0;
    std::vector<EnvelopeColumn> out(256);
    EXPECT_GT(cache_.render(1, 0, 0, 262144, 256, out.data(), out.size(), channels), 0);
    EXPECT_EQ(cache_.render(1, 1, 0, 262144, 256, out.data(), out.size(), channels), 0);
}

// Scrolling sideways must ask only for what is new, not refetch the window.
TEST_F(EnvelopeCacheTest, PartialCoverageAsksOnlyForTheGap) {
    const uint32_t total = 1u << 20;  // 1,048,576 frames
    ASSERT_TRUE(FillRun(1, 0, 0, 262144, total, 256));

    // Same zoom (same tier), shifted right by half a window.
    uint32_t s = 0, e = 0;
    uint16_t c = 0;
    ASSERT_TRUE(cache_.nextRequest(1, 0, 131072, 393216, total, 256, 1280, s, e, c));
    EXPECT_EQ(s, 262144u) << "must start where the cached run ends";
    EXPECT_EQ(e, 393216u);
}

// A run wider than one request must not be dropped or asked for in one go.
TEST_F(EnvelopeCacheTest, WideViewIsFilledOverSeveralRequests) {
    const uint32_t total = 1u << 21;
    // 2048 columns of tier data needed, but only 512 columns per request.
    ASSERT_TRUE(FillRun(1, 0, 0, total, total, 2048, 1, 512));

    uint32_t s = 0, e = 0;
    uint16_t c = 0;
    ASSERT_TRUE(cache_.nextRequest(1, 0, 0, total, total, 2048, 512, s, e, c));
    EXPECT_EQ(c, 512);
    EXPECT_GT(s, 0u) << "the second request must continue, not restart";
}

TEST_F(EnvelopeCacheTest, RenderMergesTierColumnsAndKeepsExtremes) {
    const uint32_t total = 4096;
    ASSERT_TRUE(FillRun(1, 0, 0, total, total, 1024));  // tier fpc 4, 1024 columns

    uint8_t channels = 0;
    std::vector<EnvelopeColumn> out(256);
    // A quarter of the display columns: 4096 frames over 256 columns is 16
    // frames per display column, which is 4 tier columns each.
    const uint16_t drawn = cache_.render(1, 0, 0, total, 256, out.data(), out.size(), channels);
    ASSERT_EQ(drawn, 256);
    EXPECT_EQ(channels, 1);
    // Tier column c holds (-(c+1), c+1). Display column 0 merges tier columns
    // 0..3, so it must carry the widest of the four, not the first - that is
    // the difference between an envelope and a decimation.
    EXPECT_EQ(out[0].max_sample, 4);
    EXPECT_EQ(out[0].min_sample, -4);
    EXPECT_GT(out[255].max_sample, out[0].max_sample);
}

TEST_F(EnvelopeCacheTest, StereoRunsSurviveRoundTrip) {
    const uint32_t total = 65536;
    ASSERT_TRUE(FillRun(1, 0, 0, total, total, 256, /*channels=*/2));

    uint8_t channels = 0;
    std::vector<EnvelopeColumn> out(256 * 2);
    EXPECT_EQ(cache_.render(1, 0, 0, total, 256, out.data(), out.size(), channels), 256);
    EXPECT_EQ(channels, 2);
}

TEST_F(EnvelopeCacheTest, RenderRefusesAnUndersizedOutputBuffer) {
    const uint32_t total = 65536;
    ASSERT_TRUE(FillRun(1, 0, 0, total, total, 256, /*channels=*/2));

    uint8_t channels = 0;
    std::vector<EnvelopeColumn> out(256);  // needs 256 * 2 for stereo
    EXPECT_EQ(cache_.render(1, 0, 0, total, 256, out.data(), out.size(), channels), 0);
}

// A chunk that does not match the request in flight is a reply to a view the
// user has already left. Filing it would draw the wrong audio.
TEST_F(EnvelopeCacheTest, MismatchedChunksAreDropped) {
    uint32_t req_start = 0, req_end = 0;
    uint16_t req_columns = 0;
    ASSERT_TRUE(
        cache_.nextRequest(1, 0, 0, 65536, 65536, 256, 1280, req_start, req_end, req_columns));
    cache_.noteRequest(1, 0, req_start, req_end, req_columns);

    EnvelopeChunkMessage header;
    header.sample_id = 2;  // different sample
    header.generation = 0;
    header.start_frame = req_start;
    header.end_frame = req_end;
    header.total_columns = req_columns;
    header.first_column = 0;
    header.columns = req_columns;
    header.channels = 1;
    std::vector<EnvelopeColumn> cols(req_columns);
    EXPECT_FALSE(cache_.ingest(header, cols.data()));
    EXPECT_EQ(cache_.entryCount(), 0u);
}

// The Daisy re-sends a chunk when its TX queue is full, so overlap is normal
// and must not stall the run; a genuine gap must not be accepted as filled.
TEST_F(EnvelopeCacheTest, RepeatedChunksAreToleratedButGapsAreNot) {
    uint32_t req_start = 0, req_end = 0;
    uint16_t req_columns = 0;
    ASSERT_TRUE(
        cache_.nextRequest(1, 0, 0, 65536, 65536, 256, 1280, req_start, req_end, req_columns));
    cache_.noteRequest(1, 0, req_start, req_end, req_columns);
    ASSERT_GE(req_columns, 4);

    std::vector<EnvelopeColumn> cols(req_columns, EnvelopeColumn(-1, 1));

    EnvelopeChunkMessage header;
    header.sample_id = 1;
    header.generation = 0;
    header.start_frame = req_start;
    header.end_frame = req_end;
    header.total_columns = req_columns;
    header.channels = 1;

    header.first_column = 0;
    header.columns = 2;
    EXPECT_FALSE(cache_.ingest(header, cols.data()));  // incomplete run

    header.first_column = 0;  // resend of the same columns
    EXPECT_FALSE(cache_.ingest(header, cols.data()));

    header.first_column = 3;  // skips column 2: a hole, not a resend
    header.columns = static_cast<uint16_t>(req_columns - 3);
    EXPECT_FALSE(cache_.ingest(header, cols.data()));

    header.first_column = 2;
    header.columns = static_cast<uint16_t>(req_columns - 2);
    EXPECT_TRUE(cache_.ingest(header, cols.data()));
    EXPECT_EQ(cache_.entryCount(), 1u);
}

// The budget is the point: exceeding it on a board where LVGL's draw buffers
// share the same PSRAM would trade a fast waveform for a slow UI.
TEST_F(EnvelopeCacheTest, EvictsUnderBudgetPressure) {
    EnvelopeCache small;
    small.init(8 * 1024, TestAllocator());  // room for ~2 runs of 1024 columns

    for (uint16_t id = 1; id <= 6; ++id) {
        uint32_t req_start = 0, req_end = 0;
        uint16_t req_columns = 0;
        ASSERT_TRUE(
            small.nextRequest(id, 0, 0, 4096, 4096, 1024, 1280, req_start, req_end, req_columns));
        small.noteRequest(id, 0, req_start, req_end, req_columns);

        EnvelopeChunkMessage header;
        header.sample_id = id;
        header.generation = 0;
        header.start_frame = req_start;
        header.end_frame = req_end;
        header.total_columns = req_columns;
        header.first_column = 0;
        header.columns = req_columns;
        header.channels = 1;
        std::vector<EnvelopeColumn> cols(req_columns, EnvelopeColumn(-1, 1));
        ASSERT_TRUE(small.ingest(header, cols.data()));
        EXPECT_LE(small.bytesUsed(), small.budgetBytes());
    }

    EXPECT_LE(small.bytesUsed(), 8u * 1024u);
    // The newest sample must still be there; the oldest must not.
    uint8_t channels = 0;
    std::vector<EnvelopeColumn> out(1024);
    EXPECT_GT(small.render(6, 0, 0, 4096, 1024, out.data(), out.size(), channels), 0);
    EXPECT_EQ(small.render(1, 0, 0, 4096, 1024, out.data(), out.size(), channels), 0);

    small.reset();
}

TEST_F(EnvelopeCacheTest, InvalidateSampleDropsItsEntries) {
    ASSERT_TRUE(FillRun(1, 0, 0, 65536, 65536, 256));
    ASSERT_TRUE(FillRun(2, 0, 0, 65536, 65536, 256));
    EXPECT_EQ(cache_.entryCount(), 2u);

    cache_.invalidateSample(1);
    EXPECT_EQ(cache_.entryCount(), 1u);

    uint8_t channels = 0;
    std::vector<EnvelopeColumn> out(256);
    EXPECT_EQ(cache_.render(1, 0, 0, 65536, 256, out.data(), out.size(), channels), 0);
    EXPECT_GT(cache_.render(2, 0, 0, 65536, 256, out.data(), out.size(), channels), 0);
}

// The defect behind "Sample Edit shows an empty waveform": noteRequest() arms a
// run and nextRequest() refuses everything until it commits, but a run that is
// abandoned rather than completed (send failed, or the backend silently dropped
// the scan when the sample under it was reloaded) used to leave the cache armed
// forever. The guard is not per-sample, so ONE lost run stopped the whole
// process from requesting any envelope again - and with nothing cached,
// render() returned 0 and the waveform never drew.
TEST_F(EnvelopeCacheTest, AbandonedRunDoesNotBlockLaterRequests) {
    uint32_t req_start = 0, req_end = 0;
    uint16_t req_columns = 0;
    ASSERT_TRUE(
        cache_.nextRequest(1, 0, 0, 65536, 65536, 256, 1280, req_start, req_end, req_columns));
    cache_.noteRequest(1, 0, req_start, req_end, req_columns);
    EXPECT_TRUE(cache_.requestPending());

    // While armed, nothing else can be asked for - not even a different sample.
    uint32_t s = 0, e = 0;
    uint16_t c = 0;
    EXPECT_FALSE(cache_.nextRequest(1, 0, 0, 65536, 65536, 256, 1280, s, e, c));
    EXPECT_FALSE(cache_.nextRequest(2, 0, 0, 65536, 65536, 256, 1280, s, e, c));

    // The reply never comes and the caller gives up.
    cache_.abortPending();
    EXPECT_FALSE(cache_.requestPending());

    // The cache must be usable again, for this sample and for any other.
    EXPECT_TRUE(cache_.nextRequest(1, 0, 0, 65536, 65536, 256, 1280, s, e, c));
    EXPECT_EQ(s, req_start);
    EXPECT_EQ(e, req_end);
    EXPECT_EQ(c, req_columns);
    EXPECT_TRUE(cache_.nextRequest(2, 0, 0, 65536, 65536, 256, 1280, s, e, c));
}

// Aborting must discard the partial run, not let its columns be completed by a
// later run's chunks - that would file measurements from two different windows
// under one entry.
TEST_F(EnvelopeCacheTest, AbortDiscardsPartiallyReceivedColumns) {
    uint32_t req_start = 0, req_end = 0;
    uint16_t req_columns = 0;
    ASSERT_TRUE(
        cache_.nextRequest(1, 0, 0, 65536, 65536, 256, 1280, req_start, req_end, req_columns));
    cache_.noteRequest(1, 0, req_start, req_end, req_columns);
    ASSERT_GE(req_columns, 4);

    std::vector<EnvelopeColumn> cols(req_columns, EnvelopeColumn(-1, 1));
    EnvelopeChunkMessage header;
    header.sample_id = 1;
    header.generation = 0;
    header.start_frame = req_start;
    header.end_frame = req_end;
    header.total_columns = req_columns;
    header.first_column = 0;
    header.columns = 2;  // only the head of the run arrives
    header.channels = 1;
    EXPECT_FALSE(cache_.ingest(header, cols.data()));
    EXPECT_EQ(cache_.entryCount(), 0u);

    cache_.abortPending();

    // A chunk from the dead run must not be accepted afterwards.
    header.first_column = 2;
    header.columns = static_cast<uint16_t>(req_columns - 2);
    EXPECT_FALSE(cache_.ingest(header, cols.data()));
    EXPECT_EQ(cache_.entryCount(), 0u);

    // A fresh run still commits normally.
    EXPECT_TRUE(FillRun(1, 0, 0, 65536, 65536, 256));
    EXPECT_EQ(cache_.entryCount(), 1u);
}

// abortPending() on an idle cache is a no-op, so callers can use it as an
// unconditional "nothing is in flight" assertion on page entry.
TEST_F(EnvelopeCacheTest, AbortOnIdleCacheKeepsEntries) {
    ASSERT_TRUE(FillRun(1, 0, 0, 262144, 262144, 256));
    ASSERT_EQ(cache_.entryCount(), 1u);
    EXPECT_FALSE(cache_.requestPending());

    cache_.abortPending();

    EXPECT_EQ(cache_.entryCount(), 1u);
    uint32_t s = 0, e = 0;
    uint16_t c = 0;
    EXPECT_FALSE(cache_.nextRequest(1, 0, 0, 262144, 262144, 256, 1280, s, e, c));
}
