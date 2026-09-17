#include "storage/card_format_job.hpp"

#include <gtest/gtest.h>
using namespace WaveX::Protocol;
using WaveX::Storage::CardFormatJob;
TEST(CardFormatJob, RequiresLiveConfirmationThenRunsOnlyOnce) {
    CardFormatJob job;
    job.Handle({1, 99, CARD_CONFIRM_FORMAT, {}}, 0, 7, true, false, true);
    EXPECT_EQ(job.State().error, CARD_BAD_CONFIRMATION);
    EXPECT_FALSE(job.TakeRun(1000, 7));
    job.Handle({2, 0, CARD_PREPARE_FORMAT, {}}, 1000, 7, true, false, true);
    ASSERT_EQ(job.State().state, CARD_CONFIRMATION);
    job.Handle({3, job.State().token, CARD_CONFIRM_FORMAT, {}}, 1010, 7, true, false, true);
    ASSERT_TRUE(job.Busy());
    EXPECT_FALSE(job.TakeRun(1200, 7));  // no accepted-state delivery yet
    job.Sent();
    EXPECT_TRUE(job.TakeRun(1200, 7));
    EXPECT_FALSE(job.TakeRun(1201, 7));
    job.Complete(CARD_OK, true);
    job.Handle({3, 2, CARD_CONFIRM_FORMAT, {}}, 1300, 7, true, false, true);
    EXPECT_EQ(job.State().state, CARD_DONE);
    EXPECT_FALSE(job.TakeRun(1500, 7));
    job.Handle({4, 0, CARD_GET, {}}, 1500, 7, true, false, true);
    EXPECT_EQ(job.State().completed_request_id, 3u);
}
TEST(CardFormatJob, CancelExpiryReplacementAndStorageBusyCannotErase) {
    for (int failure = 0; failure < 5; ++failure) {
        CardFormatJob job;
        job.Handle({1, 0, CARD_PREPARE_FORMAT, {}}, 0, 7, true, false, true);
        if (failure == 0)
            job.Handle({2, 0, CARD_CANCEL, {}}, 1, 7, true, false, true);
        job.Handle({3, 1, CARD_CONFIRM_FORMAT, {}},
                   failure == 1 ? 60000 : 2,
                   failure == 2 ? 8 : 7,
                   failure != 4,
                   failure == 3,
                   true);
        EXPECT_EQ(job.State().state, CARD_FAILED);
        EXPECT_EQ(job.State().token, 0u);
        job.Sent();
        EXPECT_FALSE(job.TakeRun(100000, 7));
    }
}
TEST(CardFormatJob, CardChangeAfterConfirmationCancelsPendingFormat) {
    CardFormatJob job;
    job.Handle({1, 0, CARD_PREPARE_FORMAT, {}}, 0, 7, true, false, true);
    job.Handle({2, 1, CARD_CONFIRM_FORMAT, {}}, 1, 7, true, false, true);
    job.Sent();
    EXPECT_FALSE(job.TakeRun(1000, 8));
    EXPECT_EQ(job.State().error, CARD_BAD_CONFIRMATION);
}
