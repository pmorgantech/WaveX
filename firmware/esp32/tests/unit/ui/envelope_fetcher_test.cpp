// Tests for the envelope request/receive state machine.
//
// This logic previously lived only inside ui_sample_edit_page.cpp, where it
// could not be tested at all - the page needs LVGL and the whole frontend. It
// was extracted so the sample browser could share it, and these tests are the
// thing the original never had.
//
// The defect worth pinning hardest is the one recorded in roadmap 1.5.5: a run
// abandoned WITHOUT releasing the cache leaves EnvelopeCache::noteRequest()
// armed, and its guard is not per-sample - so one dropped run stops every
// waveform in the process from loading again until reboot. Several tests below
// exist only to prove the release happens on each abandonment path.

#include "envelope_fetcher.h"

#include <gtest/gtest.h>

#include <cstdlib>
#include <vector>

using WaveX::Protocol::EnvelopeChunkMessage;
using WaveX::Protocol::EnvelopeColumn;
using wavex_ui::EnvelopeCache;
using wavex_ui::EnvelopeFetcher;

namespace {

// Captured by the injected SendFn. A function pointer cannot carry state, so
// this is file-static by necessity rather than by preference.
struct SentRequest {
    uint16_t sample_id;
    uint16_t columns;
    uint32_t start_frame;
    uint32_t end_frame;
};

std::vector<SentRequest> g_sent;
bool g_send_ok = true;

bool FakeSend(uint16_t sample_id, uint16_t columns, uint32_t start_frame, uint32_t end_frame) {
    if (!g_send_ok) {
        return false;
    }
    g_sent.push_back({sample_id, columns, start_frame, end_frame});
    return true;
}

void* TestAlloc(size_t bytes) {
    return std::malloc(bytes);
}
void TestRelease(void* p) {
    std::free(p);
}

constexpr uint16_t kDisplayColumns = 256;
constexpr uint16_t kMaxRunColumns = 480;
constexpr uint32_t kTimeoutMs = 3000;

class EnvelopeFetcherTest : public ::testing::Test {
   protected:
    void SetUp() override {
        g_sent.clear();
        g_send_ok = true;

        EnvelopeCache::Allocator alloc;
        alloc.alloc = &TestAlloc;
        alloc.release = &TestRelease;
        cache_.init(256 * 1024, alloc);

        EnvelopeFetcher::Config cfg;
        cfg.display_columns = kDisplayColumns;
        cfg.max_run_columns = kMaxRunColumns;
        cfg.timeout_ms = kTimeoutMs;
        cfg.max_retries = 3;
        fetcher_.init(cfg, &FakeSend, &cache_);
    }

    /// Delivers a whole run in one chunk, as the backend does for a run that
    /// fits a single packet.
    void deliverRun(const SentRequest& req, uint8_t channels, int16_t amplitude) {
        std::vector<EnvelopeColumn> cols(static_cast<size_t>(req.columns) * channels);
        for (auto& c: cols) {
            c = EnvelopeColumn(static_cast<int16_t>(-amplitude), amplitude);
        }

        EnvelopeChunkMessage h;
        h.sample_id = req.sample_id;
        h.generation = 0;
        h.start_frame = req.start_frame;
        h.end_frame = req.end_frame;
        h.total_columns = req.columns;
        h.first_column = 0;
        h.columns = req.columns;
        h.channels = channels;
        fetcher_.onChunk(h, cols.data());
    }

    EnvelopeCache cache_;
    EnvelopeFetcher fetcher_;
};

TEST_F(EnvelopeFetcherTest, RequestSendsAndCompletesIntoTheCache) {
    ASSERT_EQ(fetcher_.request(1, 0, 0, 48000, 48000, 0), EnvelopeFetcher::Request::Sent);
    ASSERT_EQ(g_sent.size(), 1u);
    EXPECT_TRUE(fetcher_.busy());
    EXPECT_TRUE(cache_.requestPending());

    deliverRun(g_sent[0], 2, 20000);
    EXPECT_EQ(fetcher_.service(10), EnvelopeFetcher::Service::Committed);
    EXPECT_FALSE(fetcher_.busy());
    EXPECT_FALSE(cache_.requestPending()) << "committing must release the cache arming";

    // The run is now readable back out.
    std::vector<EnvelopeColumn> out(static_cast<size_t>(kDisplayColumns) * 2);
    uint8_t channels = 0;
    EXPECT_GT(cache_.render(1, 0, 0, 48000, kDisplayColumns, out.data(), out.size(), channels), 0);
    EXPECT_EQ(channels, 2);
}

TEST_F(EnvelopeFetcherTest, SecondRequestIsRefusedWhileOneIsInFlight) {
    ASSERT_EQ(fetcher_.request(1, 0, 0, 48000, 48000, 0), EnvelopeFetcher::Request::Sent);
    EXPECT_EQ(fetcher_.request(1, 0, 0, 48000, 48000, 1), EnvelopeFetcher::Request::Busy);
    EXPECT_EQ(g_sent.size(), 1u) << "a second run was put on the wire";
}

TEST_F(EnvelopeFetcherTest, CachedViewAsksForNothing) {
    ASSERT_EQ(fetcher_.request(1, 0, 0, 48000, 48000, 0), EnvelopeFetcher::Request::Sent);
    deliverRun(g_sent[0], 1, 15000);
    ASSERT_EQ(fetcher_.service(10), EnvelopeFetcher::Service::Committed);

    // The same view again is free - which is the entire point of the cache.
    EXPECT_EQ(fetcher_.request(1, 0, 0, 48000, 48000, 20), EnvelopeFetcher::Request::AlreadyCached);
    EXPECT_EQ(g_sent.size(), 1u);
}

// A failed send must leave NOTHING armed. Arming the cache and then failing to
// send was the original defect: the cache waited forever for a reply nobody
// asked for, and because in_flight_ stayed false the timeout never ran either.
TEST_F(EnvelopeFetcherTest, FailedSendArmsNeitherHalf) {
    g_send_ok = false;
    EXPECT_EQ(fetcher_.request(1, 0, 0, 48000, 48000, 0), EnvelopeFetcher::Request::SendFailed);
    EXPECT_FALSE(fetcher_.busy());
    EXPECT_FALSE(cache_.requestPending()) << "cache armed for a request never sent";

    // And the fetcher is still usable afterwards.
    g_send_ok = true;
    EXPECT_EQ(fetcher_.request(1, 0, 0, 48000, 48000, 1), EnvelopeFetcher::Request::Sent);
}

// The defect this class inherited its shape from. A run that never answers has
// to release the cache, or every later waveform - for any sample - is refused.
TEST_F(EnvelopeFetcherTest, TimeoutReleasesTheCacheNotJustTheFetcher) {
    ASSERT_EQ(fetcher_.request(1, 0, 0, 48000, 48000, 0), EnvelopeFetcher::Request::Sent);
    ASSERT_TRUE(cache_.requestPending());

    EXPECT_EQ(fetcher_.service(kTimeoutMs - 1), EnvelopeFetcher::Service::Idle);
    EXPECT_EQ(fetcher_.service(kTimeoutMs), EnvelopeFetcher::Service::Retrying);
    EXPECT_FALSE(fetcher_.busy());
    EXPECT_FALSE(cache_.requestPending()) << "a dropped run wedged the cache for every sample";

    // A different sample can still be requested - the guard is not per-sample,
    // so this is the observable consequence of the release above.
    EXPECT_EQ(fetcher_.request(7, 0, 0, 48000, 48000, kTimeoutMs), EnvelopeFetcher::Request::Sent);
}

TEST_F(EnvelopeFetcherTest, RetriesAreBoundedThenGiveUp) {
    uint32_t now = 0;
    for (int attempt = 0; attempt < 3; ++attempt) {
        ASSERT_EQ(fetcher_.request(1, 0, 0, 48000, 48000, now), EnvelopeFetcher::Request::Sent);
        now += kTimeoutMs;
        EXPECT_EQ(fetcher_.service(now), EnvelopeFetcher::Service::Retrying)
            << "attempt " << attempt;
    }
    ASSERT_EQ(fetcher_.request(1, 0, 0, 48000, 48000, now), EnvelopeFetcher::Request::Sent);
    now += kTimeoutMs;
    EXPECT_EQ(fetcher_.service(now), EnvelopeFetcher::Service::GaveUp);
    EXPECT_FALSE(cache_.requestPending()) << "giving up must still release the cache";
}

TEST_F(EnvelopeFetcherTest, CommittingResetsTheRetryBudget) {
    ASSERT_EQ(fetcher_.request(1, 0, 0, 48000, 48000, 0), EnvelopeFetcher::Request::Sent);
    ASSERT_EQ(fetcher_.service(kTimeoutMs), EnvelopeFetcher::Service::Retrying);
    EXPECT_EQ(fetcher_.retries(), 1);

    ASSERT_EQ(fetcher_.request(1, 0, 0, 48000, 48000, kTimeoutMs), EnvelopeFetcher::Request::Sent);
    deliverRun(g_sent.back(), 1, 12000);
    ASSERT_EQ(fetcher_.service(kTimeoutMs + 1), EnvelopeFetcher::Service::Committed);
    EXPECT_EQ(fetcher_.retries(), 0) << "a slow sample would spend the budget a broken one needs";
}

TEST_F(EnvelopeFetcherTest, AbortReleasesAnArmedRun) {
    ASSERT_EQ(fetcher_.request(1, 0, 0, 48000, 48000, 0), EnvelopeFetcher::Request::Sent);
    ASSERT_TRUE(cache_.requestPending());

    fetcher_.abort();
    EXPECT_FALSE(fetcher_.busy());
    EXPECT_FALSE(cache_.requestPending());
}

TEST_F(EnvelopeFetcherTest, AbortIsSafeWithNothingArmed) {
    fetcher_.abort();
    fetcher_.abort();
    EXPECT_FALSE(fetcher_.busy());
    EXPECT_FALSE(cache_.requestPending());
}

// A chunk answering a view the caller has moved on from must not be filed.
TEST_F(EnvelopeFetcherTest, ChunkForAnotherRunIsIgnored) {
    ASSERT_EQ(fetcher_.request(1, 0, 0, 48000, 48000, 0), EnvelopeFetcher::Request::Sent);

    SentRequest wrong = g_sent[0];
    wrong.sample_id = 99;  // a different sample entirely
    deliverRun(wrong, 1, 20000);

    EXPECT_EQ(fetcher_.service(10), EnvelopeFetcher::Service::Idle)
        << "a foreign chunk completed the run";
    EXPECT_TRUE(fetcher_.busy());
}

// A run split across packets, delivered in order, must assemble.
TEST_F(EnvelopeFetcherTest, MultiChunkRunAssembles) {
    ASSERT_EQ(fetcher_.request(1, 0, 0, 480000, 480000, 0), EnvelopeFetcher::Request::Sent);
    const SentRequest req = g_sent[0];
    ASSERT_GT(req.columns, 1);

    const uint16_t half = static_cast<uint16_t>(req.columns / 2);
    const uint16_t rest = static_cast<uint16_t>(req.columns - half);

    std::vector<EnvelopeColumn> cols(req.columns);
    for (auto& c: cols) {
        c = EnvelopeColumn(-1000, 1000);
    }

    EnvelopeChunkMessage h;
    h.sample_id = req.sample_id;
    h.generation = 0;
    h.start_frame = req.start_frame;
    h.end_frame = req.end_frame;
    h.total_columns = req.columns;
    h.channels = 1;

    h.first_column = 0;
    h.columns = half;
    fetcher_.onChunk(h, cols.data());
    EXPECT_EQ(fetcher_.service(1), EnvelopeFetcher::Service::Idle) << "committed on a partial run";

    h.first_column = half;
    h.columns = rest;
    fetcher_.onChunk(h, cols.data());
    EXPECT_EQ(fetcher_.service(2), EnvelopeFetcher::Service::Committed);
}

// A gap means a chunk was lost. Filing the later one would leave the hole
// filled with whatever the staging buffer held from a previous run.
TEST_F(EnvelopeFetcherTest, ChunkLeavingAGapIsRefused) {
    ASSERT_EQ(fetcher_.request(1, 0, 0, 480000, 480000, 0), EnvelopeFetcher::Request::Sent);
    const SentRequest req = g_sent[0];
    ASSERT_GT(req.columns, 4);

    std::vector<EnvelopeColumn> cols(req.columns);
    EnvelopeChunkMessage h;
    h.sample_id = req.sample_id;
    h.generation = 0;
    h.start_frame = req.start_frame;
    h.end_frame = req.end_frame;
    h.total_columns = req.columns;
    h.channels = 1;
    h.first_column = 2;  // columns 0 and 1 never arrived
    h.columns = static_cast<uint16_t>(req.columns - 2);
    fetcher_.onChunk(h, cols.data());

    EXPECT_EQ(fetcher_.service(1), EnvelopeFetcher::Service::Idle);
    EXPECT_TRUE(fetcher_.busy());
}

TEST_F(EnvelopeFetcherTest, UninitialisedFetcherRefusesRequests) {
    EnvelopeFetcher fresh;
    EXPECT_EQ(fresh.request(1, 0, 0, 48000, 48000, 0), EnvelopeFetcher::Request::NotReady);
    EXPECT_EQ(fresh.service(0), EnvelopeFetcher::Service::Idle);
}

}  // namespace
