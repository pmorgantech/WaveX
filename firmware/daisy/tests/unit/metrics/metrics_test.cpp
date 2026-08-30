// Host tests for the message-counter metrics (src/metrics/metrics.cpp).
//
// The counter is a plain extern volatile uint32_t, so the tests reset it
// directly in SetUp() and assert EXACT values - the previous version of this
// suite could only compare "after >= before" style deltas (and asserted
// unsigned >= 0 in several places, which nothing can fail).

#include "../src/metrics/metrics.h"

#include <gtest/gtest.h>

#include <cstdint>

using namespace WaveX::Metrics;

class MetricsTest : public ::testing::Test {
   protected:
    void SetUp() override {
        // The counter is process-global state; pin it to a known value so
        // every test asserts exact counts instead of relative deltas.
        g_message_count = 0;
    }

    void TearDown() override { g_message_count = 0; }
};

TEST_F(MetricsTest, GetReadsTheCounterVerbatim) {
    EXPECT_EQ(GetMessageCount(), 0u);
    g_message_count = 12345u;
    EXPECT_EQ(GetMessageCount(), 12345u);
}

TEST_F(MetricsTest, IncrementAddsExactlyOne) {
    IncrementMessageCount();
    EXPECT_EQ(GetMessageCount(), 1u);
    IncrementMessageCount();
    EXPECT_EQ(GetMessageCount(), 2u);
}

TEST_F(MetricsTest, ManyIncrementsCountExactly) {
    const uint32_t n = 1000;
    for (uint32_t i = 0; i < n; ++i) {
        IncrementMessageCount();
    }
    EXPECT_EQ(GetMessageCount(), n);
}

// The counter must wrap modulo 2^32 rather than saturate or trap - it is a
// free-running diagnostic counter, and consumers diff successive reads.
TEST_F(MetricsTest, WrapsAroundAtUint32Max) {
    g_message_count = UINT32_MAX;
    IncrementMessageCount();
    EXPECT_EQ(GetMessageCount(), 0u);
    IncrementMessageCount();
    EXPECT_EQ(GetMessageCount(), 1u);
}

TEST_F(MetricsTest, GetDoesNotModifyTheCounter) {
    g_message_count = 77u;
    EXPECT_EQ(GetMessageCount(), 77u);
    EXPECT_EQ(GetMessageCount(), 77u);
    EXPECT_EQ(g_message_count, 77u);
}

// NOTE: earlier revisions carried four DISABLED_ placeholder tests here (CPU
// load, heap fragmentation, SPI/I2C self-test, watchdog recovery). Those
// features do not exist anywhere in src/metrics/ - the placeholders asserted
// nothing and could never be enabled, so they were removed rather than kept
// as permanently-skipped noise. When such a feature lands, its tests belong
// next to its implementation, written against the real API.
