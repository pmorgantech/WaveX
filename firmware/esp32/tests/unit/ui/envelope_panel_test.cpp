// Tests for the shared waveform panel: the request/receive/render cycle that
// every page showing a waveform used to carry its own copy of.
//
// The fakes stand in for the two things the panel is kept ignorant of: the
// link (a function pair, as on the target) and the sinks (WaveformViews there,
// recorders here). What these pin is the lifecycle the pages got wrong when
// each had its own - the listener slot taken and released in the right order,
// the settle that stops a scrub flooding the link, views of different widths
// each getting their own request, and a view already complete not being
// rendered again for the sake of another.

#include "envelope_panel.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdlib>
#include <vector>

using WaveX::Protocol::EnvelopeChunkMessage;
using WaveX::Protocol::EnvelopeColumn;
using wavex_ui::EnvelopeCache;
using wavex_ui::EnvelopePanel;
using wavex_ui::EnvelopeSink;

namespace {

struct SentRequest {
    uint16_t sample_id;
    uint16_t columns;
    uint32_t start_frame;
    uint32_t end_frame;
};

// Captured by the injected link. Function pointers cannot carry state, so
// these are file-static by necessity.
std::vector<SentRequest> g_sent;
bool g_send_ok = true;
EnvelopePanel::ChunkCb g_listener = nullptr;
void* g_listener_user = nullptr;
int g_listen_calls = 0;

bool FakeSend(uint16_t sample_id, uint16_t columns, uint32_t start_frame, uint32_t end_frame) {
    if (!g_send_ok) {
        return false;
    }
    g_sent.push_back({sample_id, columns, start_frame, end_frame});
    return true;
}

void FakeListen(EnvelopePanel::ChunkCb cb, void* user) {
    g_listener = cb;
    g_listener_user = user;
    ++g_listen_calls;
}

void* TestAlloc(size_t bytes) {
    return std::malloc(bytes);
}
void TestRelease(void* p) {
    std::free(p);
}

/// Records what it is handed, as a WaveformView would draw it.
struct FakeSink : EnvelopeSink {
    explicit FakeSink(uint16_t columns) : width(columns) {}

    uint16_t columns() const override { return width; }
    void setEnvelope(const EnvelopeColumn* columns, uint16_t count, uint8_t channels) override {
        ++sets;
        last_count = count;
        last_channels = channels;
        last.assign(columns, columns + static_cast<size_t>(count) * channels);
    }
    void clear() override {
        ++clears;
        last.clear();
        last_count = 0;
        last_channels = 0;
    }

    uint16_t width;
    int sets = 0;
    int clears = 0;
    uint16_t last_count = 0;
    uint8_t last_channels = 0;
    std::vector<EnvelopeColumn> last;
};

constexpr uint32_t kSettleMs = 150;
constexpr uint32_t kTimeoutMs = 3000;
constexpr uint32_t kTotal = 480000;
// Exactly 256 frames per column at 1240 columns, so the wide view's whole
// window fits one run and the three-view tests can watch the views take
// turns rather than the first view's second run.
constexpr uint32_t kTotalOneRun = 1240u * 256u;

// Columns the cache asks for to cover [start, end) at `display` columns: the
// tier is the power of two at or below one display column, so a run carries
// between `display` and twice that. What the panel sends is the cache's
// arithmetic, not the sink's width, and these tests should say so.
uint16_t tierColumns(uint32_t start, uint32_t end, uint16_t display, uint32_t total) {
    const uint32_t fpc = EnvelopeCache::tierFramesPerColumn(end - start, display);
    const uint32_t c0 = start / fpc;
    const uint32_t c1 = std::min((end + fpc - 1) / fpc, (total + fpc - 1) / fpc);
    return static_cast<uint16_t>(c1 - c0);
}

class EnvelopePanelTest : public ::testing::Test {
   protected:
    void SetUp() override {
        g_sent.clear();
        g_send_ok = true;
        g_listener = nullptr;
        g_listener_user = nullptr;
        g_listen_calls = 0;

        EnvelopeCache::Allocator alloc;
        alloc.alloc = &TestAlloc;
        alloc.release = &TestRelease;
        cache_.init(512 * 1024, alloc);

        link_.send = &FakeSend;
        link_.listen = &FakeListen;

        config_.settle_ms = kSettleMs;
        config_.timeout_ms = kTimeoutMs;
        config_.max_retries = 2;
    }

    void TearDown() override { panel_.detach(); }

    void attachOne(uint16_t columns) {
        sinks_.emplace_back(columns);
        EnvelopeSink* s[] = {&sinks_[0]};
        panel_.attach(config_, link_, &cache_, s, 1);
    }

    void attachThree(uint16_t wide, uint16_t half) {
        sinks_.emplace_back(wide);
        sinks_.emplace_back(half);
        sinks_.emplace_back(half);
        EnvelopeSink* s[] = {&sinks_[0], &sinks_[1], &sinks_[2]};
        panel_.attach(config_, link_, &cache_, s, 3);
    }

    /// Answers a request through the listener the panel registered, the way
    /// the RX task does: one chunk carrying the whole run.
    void deliver(const SentRequest& req,
                 uint8_t channels,
                 int16_t amplitude,
                 uint32_t total_frames = kTotal) {
        ASSERT_NE(g_listener, nullptr);
        std::vector<EnvelopeColumn> cols(static_cast<size_t>(req.columns) * channels);
        for (auto& c: cols) {
            c = EnvelopeColumn(static_cast<int16_t>(-amplitude), amplitude);
        }
        EnvelopeChunkMessage h;
        h.sample_id = req.sample_id;
        h.generation = 0;
        h.start_frame = req.start_frame;
        // PumpEnvelopeJob echoes its EOF-clamped end, not the rounded request.
        h.end_frame = std::min(req.end_frame, total_frames);
        h.total_columns = req.columns;
        h.first_column = 0;
        h.columns = req.columns;
        h.channels = channels;
        g_listener(h, cols.data(), g_listener_user);
    }

    EnvelopeCache cache_;
    EnvelopePanel panel_;
    EnvelopePanel::Link link_;
    EnvelopePanel::Config config_;
    std::vector<FakeSink> sinks_;
};

TEST_F(EnvelopePanelTest, AttachTakesTheListenerAndDetachReleasesIt) {
    attachOne(256);
    EXPECT_TRUE(panel_.attached());
    EXPECT_NE(g_listener, nullptr);
    EXPECT_EQ(g_listener_user, &panel_);

    panel_.detach();
    EXPECT_FALSE(panel_.attached());
    EXPECT_EQ(g_listener, nullptr);
    EXPECT_EQ(g_listener_user, nullptr);

    // Twice is fine, and does not touch the slot again.
    const int calls = g_listen_calls;
    panel_.detach();
    EXPECT_EQ(g_listen_calls, calls);
}

TEST_F(EnvelopePanelTest, NothingIsAskedForWithoutASample) {
    attachOne(256);
    panel_.setWindow(0, 0, kTotal);
    for (uint32_t t = 0; t < 1000; t += 50) {
        EXPECT_EQ(panel_.service(t), EnvelopePanel::Event::None);
    }
    EXPECT_TRUE(g_sent.empty());
}

TEST_F(EnvelopePanelTest, SetSampleClearsTheSinkAndRequestsAtOnce) {
    attachOne(256);
    panel_.setWindow(0, 0, kTotal);
    panel_.setSample(7, 0, kTotal);
    EXPECT_EQ(sinks_[0].clears, 1);

    // Immediately: no settle for a sample that has just been named.
    panel_.service(0);
    ASSERT_EQ(g_sent.size(), 1u);
    EXPECT_EQ(g_sent[0].sample_id, 7);
    EXPECT_EQ(g_sent[0].columns, tierColumns(0, kTotal, 256, kTotal));
    EXPECT_GE(g_sent[0].columns, 256);  // never coarser than the sink
    EXPECT_LT(g_sent[0].columns, 512);  // never more than a tier finer
    EXPECT_TRUE(panel_.busy());
}

TEST_F(EnvelopePanelTest, ALandedRunIsRenderedAtTheSinksWidth) {
    attachOne(256);
    panel_.setWindow(0, 0, kTotal);
    panel_.setSample(1, 0, kTotal);
    panel_.service(0);
    ASSERT_EQ(g_sent.size(), 1u);

    deliver(g_sent[0], 2, 12000);
    EXPECT_EQ(panel_.service(10), EnvelopePanel::Event::Drawn);
    EXPECT_FALSE(panel_.busy());
    EXPECT_EQ(sinks_[0].sets, 1);
    EXPECT_EQ(sinks_[0].last_count, 256);
    EXPECT_EQ(sinks_[0].last_channels, 2);
    ASSERT_EQ(sinks_[0].last.size(), 512u);
    ASSERT_FALSE(sinks_[0].last.empty());
    EXPECT_EQ(sinks_[0].last[0].max_sample, 12000);
    EXPECT_TRUE(panel_.drawn(0));

    // Complete: nothing more goes out, and nothing is rendered again.
    for (uint32_t t = 20; t < 2000; t += 50) {
        EXPECT_EQ(panel_.service(t), EnvelopePanel::Event::None);
    }
    EXPECT_EQ(g_sent.size(), 1u);
    EXPECT_EQ(sinks_[0].sets, 1);
}

TEST_F(EnvelopePanelTest, AMovingWindowWaitsForTheSettle) {
    attachOne(256);
    panel_.setWindow(0, 0, kTotal);
    panel_.setSample(1, 0, kTotal);
    panel_.service(0);
    ASSERT_EQ(g_sent.size(), 1u);
    deliver(g_sent[0], 1, 1000);
    panel_.service(10);
    ASSERT_TRUE(panel_.drawn(0));

    // A scrub: the window moves on every tick for a while.
    uint32_t now = 20;
    for (int step = 1; step <= 10; ++step, now += 20) {
        panel_.setWindow(0, step * 1000, step * 1000 + 48000);
        panel_.service(now);
    }
    EXPECT_EQ(g_sent.size(), 1u) << "asked for a window the user had already left";

    // It stops; the settle elapses; ONE request goes out for where it ended.
    panel_.service(now + kSettleMs);
    ASSERT_EQ(g_sent.size(), 2u);
    EXPECT_EQ(g_sent[1].sample_id, 1);
    EXPECT_LE(g_sent[1].start_frame, 10000u);
    EXPECT_GE(g_sent[1].end_frame, 58000u);
}

TEST_F(EnvelopePanelTest, RequestNowSkipsTheSettle) {
    attachOne(256);
    panel_.setWindow(0, 0, kTotal);
    panel_.setSample(1, 0, kTotal);
    panel_.service(0);
    deliver(g_sent[0], 1, 1000);
    panel_.service(10);

    panel_.setWindow(0, 1000, 49000);
    panel_.requestNow();
    panel_.service(20);
    EXPECT_EQ(g_sent.size(), 2u);
}

TEST_F(EnvelopePanelTest, EachViewIsRequestedAtItsOwnWidthInTurn) {
    attachThree(1240, 616);
    panel_.setWindow(0, 0, kTotalOneRun);
    panel_.setWindow(1, 200000, 210000);
    panel_.setWindow(2, 300000, 310000);
    panel_.setSample(3, 0, kTotalOneRun);

    panel_.service(0);
    ASSERT_EQ(g_sent.size(), 1u);
    EXPECT_EQ(g_sent[0].columns, 1240);  // the wide one first
    deliver(g_sent[0], 1, 2000);
    EXPECT_EQ(panel_.service(10), EnvelopePanel::Event::Drawn);
    EXPECT_EQ(sinks_[0].sets, 1);
    EXPECT_TRUE(panel_.drawn(0));
    // The halves show the whole-file run as a stand-in at once, and are not
    // counted done: their own tier is sixteen times finer.
    EXPECT_EQ(sinks_[1].sets, 1);
    EXPECT_EQ(sinks_[2].sets, 1);
    EXPECT_FALSE(panel_.drawn(1));
    EXPECT_FALSE(panel_.drawn(2));

    // The next view's run went out in the same tick as the first landed, at
    // its own width.
    ASSERT_EQ(g_sent.size(), 2u);
    EXPECT_EQ(g_sent[1].columns, tierColumns(200000, 210000, 616, kTotalOneRun));
    EXPECT_LE(g_sent[1].start_frame, 200000u);
    deliver(g_sent[1], 1, 2000);
    panel_.service(20);
    EXPECT_TRUE(panel_.drawn(1));
    EXPECT_EQ(sinks_[1].sets, 2);  // the stand-in replaced by the real thing
    ASSERT_EQ(g_sent.size(), 3u);
    EXPECT_EQ(g_sent[2].columns, tierColumns(300000, 310000, 616, kTotalOneRun));
    EXPECT_LE(g_sent[2].start_frame, 300000u);
    deliver(g_sent[2], 1, 2000);
    panel_.service(30);
    EXPECT_TRUE(panel_.drawn(2));
    EXPECT_EQ(sinks_[2].sets, 2);

    // The wide view was rendered once: the halves landing did not redraw it.
    // And the halves, at one tier and far apart, both stay resident - nothing
    // more is asked for however long the page sits here.
    for (uint32_t t = 40; t < 5000; t += 50) {
        panel_.service(t);
    }
    EXPECT_EQ(sinks_[0].sets, 1);
    EXPECT_EQ(sinks_[1].sets, 2);
    EXPECT_EQ(sinks_[2].sets, 2);
    EXPECT_EQ(g_sent.size(), 3u);
}

TEST_F(EnvelopePanelTest, DisabledViewsAreNeitherRequestedNorRendered) {
    attachThree(1240, 616);
    panel_.setWindow(0, 0, kTotalOneRun);
    panel_.setWindow(1, 200000, 210000);
    panel_.setWindow(2, 300000, 310000);
    panel_.enableView(1, false);
    panel_.enableView(2, false);
    panel_.setSample(3, 0, kTotalOneRun);

    panel_.service(0);
    deliver(g_sent[0], 1, 2000);
    panel_.service(10);
    for (uint32_t t = 20; t < 1000; t += 50) {
        panel_.service(t);
    }
    EXPECT_EQ(g_sent.size(), 1u);
    EXPECT_EQ(sinks_[1].sets, 0);
    EXPECT_EQ(sinks_[2].sets, 0);

    // Enabled again: rendered from the cache if it can be, requested after
    // the settle for what it cannot.
    panel_.enableView(1, true);
    panel_.service(1000);
    EXPECT_EQ(g_sent.size(), 1u);
    panel_.service(1000 + kSettleMs);
    ASSERT_EQ(g_sent.size(), 2u);
    EXPECT_EQ(g_sent[1].columns, tierColumns(200000, 210000, 616, kTotalOneRun));
}

TEST_F(EnvelopePanelTest, ATimeoutRetriesAfterTheSettleAndThenGivesUp) {
    attachOne(256);
    panel_.setWindow(0, 0, kTotal);
    panel_.setSample(1, 0, kTotal);
    panel_.service(0);
    ASSERT_EQ(g_sent.size(), 1u);

    EXPECT_EQ(panel_.service(kTimeoutMs), EnvelopePanel::Event::Retrying);
    EXPECT_FALSE(panel_.busy());
    EXPECT_EQ(g_sent.size(), 1u);
    panel_.service(kTimeoutMs + kSettleMs);
    ASSERT_EQ(g_sent.size(), 2u);

    EXPECT_EQ(panel_.service(2 * kTimeoutMs + kSettleMs), EnvelopePanel::Event::Retrying);
    panel_.service(2 * kTimeoutMs + 2 * kSettleMs);
    ASSERT_EQ(g_sent.size(), 3u);

    EXPECT_EQ(panel_.service(3 * kTimeoutMs + 2 * kSettleMs), EnvelopePanel::Event::GaveUp);
    EXPECT_TRUE(panel_.gaveUp());
    for (uint32_t t = 3 * kTimeoutMs + 2 * kSettleMs; t < 20000; t += 100) {
        EXPECT_EQ(panel_.service(t), EnvelopePanel::Event::None);
    }
    EXPECT_EQ(g_sent.size(), 3u);

    // The cache's one run was released on the way: a fresh sample requests.
    panel_.setSample(2, 0, kTotal);
    EXPECT_FALSE(panel_.gaveUp());
    panel_.service(20000);
    ASSERT_EQ(g_sent.size(), 4u);
    EXPECT_EQ(g_sent[3].sample_id, 2);
}

TEST_F(EnvelopePanelTest, ARefusedSendIsRetriedThenGivenUpOn) {
    attachOne(256);
    panel_.setWindow(0, 0, kTotal);
    panel_.setSample(1, 0, kTotal);
    g_send_ok = false;

    EXPECT_EQ(panel_.service(0), EnvelopePanel::Event::SendFailed);
    EXPECT_FALSE(panel_.busy());
    EXPECT_EQ(panel_.service(kSettleMs), EnvelopePanel::Event::SendFailed);
    EXPECT_EQ(panel_.service(2 * kSettleMs), EnvelopePanel::Event::GaveUp);
    EXPECT_TRUE(panel_.gaveUp());

    // Nothing was ever armed in the cache, so a new sample can still ask.
    g_send_ok = true;
    panel_.setSample(2, 0, kTotal);
    panel_.service(3 * kSettleMs);
    ASSERT_EQ(g_sent.size(), 1u);
    EXPECT_EQ(g_sent[0].sample_id, 2);
}

TEST_F(EnvelopePanelTest, DetachReleasesTheRunSoTheNextPanelCanAsk) {
    attachOne(256);
    panel_.setWindow(0, 0, kTotal);
    panel_.setSample(1, 0, kTotal);
    panel_.service(0);
    ASSERT_TRUE(panel_.busy());
    panel_.detach();

    // Another page's panel on the same cache.
    EnvelopePanel other;
    FakeSink sink(300);
    EnvelopeSink* s[] = {&sink};
    other.attach(config_, link_, &cache_, s, 1);
    other.setWindow(0, 0, kTotal);
    other.setSample(9, 0, kTotal);
    other.service(0);
    ASSERT_EQ(g_sent.size(), 2u);
    EXPECT_EQ(g_sent[1].sample_id, 9);
    other.detach();
}

TEST_F(EnvelopePanelTest, AChunkForTheOldSampleIsNotFiledAgainstTheNew) {
    attachOne(256);
    panel_.setWindow(0, 0, kTotal);
    panel_.setSample(1, 0, kTotal);
    panel_.service(0);
    const SentRequest old = g_sent[0];

    panel_.setSample(2, 0, kTotal);
    panel_.service(10);
    ASSERT_EQ(g_sent.size(), 2u);
    deliver(old, 1, 30000);  // late reply to the sample just left
    EXPECT_EQ(panel_.service(20), EnvelopePanel::Event::None);
    EXPECT_TRUE(panel_.busy());
    EXPECT_EQ(sinks_[0].sets, 0);

    deliver(g_sent[1], 1, 5000);
    EXPECT_EQ(panel_.service(30), EnvelopePanel::Event::Drawn);
    ASSERT_FALSE(sinks_[0].last.empty());
    EXPECT_EQ(sinks_[0].last[0].max_sample, 5000);
}

TEST_F(EnvelopePanelTest, ClearSampleClearsAndStopsAsking) {
    attachOne(256);
    panel_.setWindow(0, 0, kTotal);
    panel_.setSample(1, 0, kTotal);
    panel_.service(0);
    deliver(g_sent[0], 1, 1000);
    panel_.service(10);
    EXPECT_EQ(sinks_[0].sets, 1);

    panel_.clearSample();
    EXPECT_EQ(sinks_[0].clears, 2);  // once at setSample, once now
    EXPECT_FALSE(panel_.hasSample());
    for (uint32_t t = 20; t < 1000; t += 50) {
        EXPECT_EQ(panel_.service(t), EnvelopePanel::Event::None);
    }
    EXPECT_EQ(g_sent.size(), 1u);
}

TEST_F(EnvelopePanelTest, RedrawRendersACompleteViewAgain) {
    attachOne(256);
    panel_.setWindow(0, 0, kTotal);
    panel_.setSample(1, 0, kTotal);
    panel_.service(0);
    deliver(g_sent[0], 1, 1000);
    panel_.service(10);
    ASSERT_EQ(sinks_[0].sets, 1);

    panel_.redraw();
    EXPECT_EQ(panel_.service(20), EnvelopePanel::Event::Drawn);
    EXPECT_EQ(sinks_[0].sets, 2);
    EXPECT_EQ(g_sent.size(), 1u);
}

TEST_F(EnvelopePanelTest, CachedWindowDrawsBeforeSettleWithoutSending) {
    attachOne(256);
    panel_.setWindow(0, 0, kTotal);
    panel_.setSample(1, 0, kTotal);
    panel_.service(0);
    deliver(g_sent[0], 2, 1234);
    panel_.service(10);
    const int before = sinks_[0].sets;
    const size_t requests = g_sent.size();
    panel_.setWindow(0, 1000, 49000);
    panel_.service(20);
    EXPECT_EQ(sinks_[0].sets, before + 1);
    EXPECT_EQ(g_sent.size(), requests);
    EXPECT_EQ(sinks_[0].last_channels, 2);
    panel_.service(20 + kSettleMs);
    EXPECT_EQ(g_sent.size(), requests + 1);  // finer detail still requested
}

}  // namespace
