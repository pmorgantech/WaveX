#include "attn_watchdog.hpp"

#include <gtest/gtest.h>

using WaveX::Protocol::AttnWatchdog;

TEST(AttnWatchdogTest, NotAssertedInitially) {
    AttnWatchdog w;
    EXPECT_FALSE(w.IsAsserted());
    EXPECT_FALSE(w.ShouldForceDeassert(1'000'000));
}

TEST(AttnWatchdogTest, ClearedBeforeThresholdDoesNotTrip) {
    AttnWatchdog w;
    w.MarkAsserted(1000);
    EXPECT_FALSE(w.ShouldForceDeassert(1000 + AttnWatchdog::kForceDeassertMs - 1));
    w.MarkCleared();
    EXPECT_FALSE(w.IsAsserted());
    EXPECT_FALSE(w.ShouldForceDeassert(1000 + AttnWatchdog::kForceDeassertMs + 1000));
}

// The actual regression test for "ATTN stuck-high recovery": if
// spi_post_trans_cb never fires (the Daisy never clocks the transaction -
// it rebooted, its EXTI handler missed the edge, etc.), the watchdog must
// eventually say "force it low" instead of staying silently wedged forever.
TEST(AttnWatchdogTest, TripsAfterForceDeassertThreshold) {
    AttnWatchdog w;
    w.MarkAsserted(1000);

    EXPECT_FALSE(w.ShouldForceDeassert(1000 + AttnWatchdog::kForceDeassertMs - 1))
        << "must not trip early";
    EXPECT_TRUE(w.ShouldForceDeassert(1000 + AttnWatchdog::kForceDeassertMs))
        << "must trip once the threshold elapses";
    EXPECT_TRUE(w.ShouldForceDeassert(1000 + AttnWatchdog::kForceDeassertMs + 10000))
        << "stays tripped indefinitely until MarkCleared()";
}

TEST(AttnWatchdogTest, RepeatedMarkAssertedDoesNotResetClock) {
    AttnWatchdog w;
    w.MarkAsserted(1000);
    // A caller re-signaling "still asserted" (e.g. re-queueing while ATTN
    // is already high) must not push the deadline back out - only the
    // first assertion in a stuck streak starts the timer.
    w.MarkAsserted(1000 + AttnWatchdog::kForceDeassertMs - 1);

    EXPECT_TRUE(w.ShouldForceDeassert(1000 + AttnWatchdog::kForceDeassertMs));
}

TEST(AttnWatchdogTest, ClearThenReassertStartsANewWindow) {
    AttnWatchdog w;
    w.MarkAsserted(1000);
    w.MarkCleared();

    w.MarkAsserted(5000);
    EXPECT_FALSE(w.ShouldForceDeassert(5000 + AttnWatchdog::kForceDeassertMs - 1));
    EXPECT_TRUE(w.ShouldForceDeassert(5000 + AttnWatchdog::kForceDeassertMs));
}
