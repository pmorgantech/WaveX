#include "ui/record_assignment.h"

#include <gtest/gtest.h>
using namespace WaveX::Protocol;
using wavex_ui::RecordAssignment;
TEST(RecordAssignmentTest, RequiresSavedReleasedTakeAndMatchingAcknowledgement) {
    RecordAssignment intent;
    RecordStatusMessage take;
    take.state = REC_READY;
    take.sample_id = 1030;
    EXPECT_FALSE(intent.Begin(take, 42, true));
    detail::CopyWireString(take.path, sizeof(take.path), "/wavex/recordings/take.wav");
    take.active_request_id = 9;
    EXPECT_FALSE(intent.Begin(take, 42, true));
    take.active_request_id = 0;
    ASSERT_TRUE(intent.Begin(take, 42, true));
    EXPECT_EQ(intent.sample, 1030);
    EXPECT_TRUE(intent.keyboard);
    RecordStatusMessage ack;
    ack.state = REC_IDLE;
    ack.completed_op = REC_DISCARD;
    ack.completed_request_id = 41;
    EXPECT_FALSE(intent.Complete(ack));
    ack.completed_request_id = 42;
    ack.error = REC_STALE;
    EXPECT_FALSE(intent.Complete(ack));
    ack.error = REC_OK;
    ack.state = REC_READY;
    EXPECT_FALSE(intent.Complete(ack));
    ack.state = REC_IDLE;
    EXPECT_TRUE(intent.Complete(ack));
    intent = {};
    EXPECT_FALSE(intent.Complete(ack));
}
