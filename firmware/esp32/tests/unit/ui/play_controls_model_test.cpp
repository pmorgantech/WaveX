#include "ui/play_controls_model.h"

#include <gtest/gtest.h>
using namespace WaveX::Protocol;
TEST(PlayControlsModel, RejectsStaleIdentityAndExpiresConfirmedValues) {
    wavex_ui::PlayControlsModel model;
    model.Reset(2);
    model.Requested(0, 10, 100);
    InstEditSyncMessage reply;
    reply.request_id = 9;
    reply.track = 2;
    reply.valid = 1;
    reply.sound.cutoff_hz = 200;
    EXPECT_FALSE(model.Accept(reply, 101));
    reply.request_id = 10;
    reply.track = 1;
    EXPECT_FALSE(model.Accept(reply, 101));
    reply.track = 2;
    ASSERT_TRUE(model.Accept(reply, 101));
    EXPECT_NEAR(model.Value(0), 21845, 1);
    EXPECT_TRUE(model.Ready(0, 1600));
    EXPECT_FALSE(model.Ready(0, 1601));
    model.Reset(3);
    EXPECT_FALSE(model.Accept(reply, 2000));
    EXPECT_FALSE(model.Ready(0, 2000));
}
TEST(PlayControlsModel, EditInvalidatesReadinessAndOldOutstandingReply) {
    wavex_ui::PlayControlsModel model;
    model.Requested(0, 1, 0);
    InstEditSyncMessage reply;
    reply.request_id = 1;
    reply.valid = 1;
    ASSERT_TRUE(model.Accept(reply, 1));
    model.Requested(0, 2, 20);
    model.Edited(0);
    reply.request_id = 2;
    EXPECT_FALSE(model.Accept(reply, 21));
    EXPECT_FALSE(model.Ready(0, 21));
    EXPECT_TRUE(model.Due(0, 21));
}
TEST(PlayControlsModel, EnvelopeReadbackAndWireRangeAreExplicit) {
    wavex_ui::PlayControlsModel model;
    model.Requested(1, 42, 0);
    InstModSyncMessage reply;
    reply.request_id = 42;
    reply.valid = 1;
    reply.envelopes[0] = {.501f, .101f, .25f, 12.f};
    ASSERT_TRUE(model.Accept(reply, 1));
    EXPECT_NEAR(model.Value(2), 16384, 1);
    EXPECT_NEAR(model.Value(3), 3277, 1);
    EXPECT_NEAR(model.Value(4), 16384, 1);
    EXPECT_TRUE(model.Ready(2, 2));
    EXPECT_FALSE(model.Ready(5, 2));  // Never jump 12-second release to the 2-second CC limit.
}
