#include "ui/sample_load_failure.h"

#include <gtest/gtest.h>

TEST(SampleLoadFailureTest, BusyReplyExplainsHowToRetryInsteadOfClaimingMemoryFailure) {
    EXPECT_STREQ(wavex_ui::sampleLoadFailureText(WaveX::Protocol::SAMPLE_LOAD_FAIL_BUSY),
                 "another sample import is still running - try again when it finishes");
}

TEST(SampleLoadFailureTest, UnrecognizedWireReasonRetainsTheFallback) {
    EXPECT_STREQ(wavex_ui::sampleLoadFailureText(UINT32_MAX), "unknown reason");
}
