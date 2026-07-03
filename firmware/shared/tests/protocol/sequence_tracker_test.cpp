#include "sequence_tracker.hpp"

#include <gtest/gtest.h>

using WaveX::Protocol::SequenceTracker;

TEST(SequenceTrackerTest, AcceptsSimpleIncreasingSequence) {
    SequenceTracker t;
    for (uint16_t seq = 1; seq <= 20; ++seq) {
        EXPECT_EQ(t.Evaluate(seq), SequenceTracker::Result::Accept) << "seq=" << seq;
    }
}

TEST(SequenceTrackerTest, RejectsSequenceZero) {
    SequenceTracker t;
    EXPECT_EQ(t.Evaluate(0), SequenceTracker::Result::OutOfOrder);
}

TEST(SequenceTrackerTest, RejectsExactDuplicate) {
    SequenceTracker t;
    ASSERT_EQ(t.Evaluate(5), SequenceTracker::Result::Accept);
    EXPECT_EQ(t.Evaluate(5), SequenceTracker::Result::Duplicate);
    EXPECT_EQ(t.DuplicateCount(), 1u);
}

TEST(SequenceTrackerTest, ToleratesMinorReordering) {
    SequenceTracker t;
    for (uint16_t seq = 1; seq <= 20; ++seq) {
        ASSERT_EQ(t.Evaluate(seq), SequenceTracker::Result::Accept);
    }
    // expected_seq_ is now 21; a packet up to kReorderTolerance (10) behind
    // is still accepted as ordinary reordering, not rejected.
    EXPECT_EQ(t.Evaluate(15), SequenceTracker::Result::Accept);
}

TEST(SequenceTrackerTest, RejectsSevereOutOfOrderEarlyInSession) {
    SequenceTracker t;
    for (uint16_t seq = 1; seq <= 15; ++seq) {
        ASSERT_EQ(t.Evaluate(seq), SequenceTracker::Result::Accept);
    }
    // seq=3 is far enough behind expected (16) to fail the reorder-tolerance
    // check, but expected_seq_ (16) hasn't advanced past
    // kResyncMinPriorProgress (100) yet, so this must NOT be misclassified
    // as a peer reboot - it's ordinary corruption/severe reordering this
    // early in a session.
    EXPECT_EQ(t.Evaluate(3), SequenceTracker::Result::OutOfOrder);
    EXPECT_EQ(t.OutOfOrderCount(), 1u);
    EXPECT_EQ(t.ResyncCount(), 0u);
}

// This is the actual regression test for the bug roadmap item 7 asks for:
// a peer rebooting mid-session resets its sequence counter to 1 (0 is
// reserved - see protocol.h/uart_protocol.h), and the old duplicated
// is_duplicate_packet() logic in daisy_spi_link.cpp/esp_spi_link.cpp would
// have permanently classified every packet the rebooted peer ever sent
// again as "out-of-order" and dropped it - a wedge with no recovery path.
TEST(SequenceTrackerTest, PeerRebootMidSessionResyncsInsteadOfWedging) {
    SequenceTracker t;
    // Establish a long-running session, well past kResyncMinPriorProgress.
    for (uint16_t seq = 1; seq <= 500; ++seq) {
        ASSERT_EQ(t.Evaluate(seq), SequenceTracker::Result::Accept) << "seq=" << seq;
    }
    ASSERT_EQ(t.ExpectedSeq(), 501);

    // Peer reboots; its next packet is seq=1 again.
    EXPECT_EQ(t.Evaluate(1), SequenceTracker::Result::ResyncAccept);
    EXPECT_EQ(t.ResyncCount(), 1u);
    EXPECT_EQ(t.ExpectedSeq(), 2);

    // Communication must continue normally after the resync, not stay wedged.
    EXPECT_EQ(t.Evaluate(2), SequenceTracker::Result::Accept);
    EXPECT_EQ(t.Evaluate(3), SequenceTracker::Result::Accept);
    for (uint16_t seq = 4; seq <= 50; ++seq) {
        EXPECT_EQ(t.Evaluate(seq), SequenceTracker::Result::Accept) << "seq=" << seq;
    }
}

TEST(SequenceTrackerTest, RebootResyncOnlyRecognizedForFreshLookingSequence) {
    SequenceTracker t;
    for (uint16_t seq = 1; seq <= 500; ++seq) {
        ASSERT_EQ(t.Evaluate(seq), SequenceTracker::Result::Accept);
    }
    // seq=50 is far below expectation (like a reboot-flavored drop would
    // be) but isn't "fresh" (> kFreshStartMax) - this is corruption or a
    // severe reorder, not a reboot signature, and must not resync.
    EXPECT_EQ(t.Evaluate(50), SequenceTracker::Result::OutOfOrder);
    EXPECT_EQ(t.ResyncCount(), 0u);
}
