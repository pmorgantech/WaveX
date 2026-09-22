#include "ui/sample_binding_model.h"

#include <gtest/gtest.h>
using WaveX::Protocol::TrackStateMessage;
using wavex_ui::SampleBindingModel;
TEST(SampleBindingModel, RetainsRejectedSendAndWaitsForMatchingTrackConfirmation) {
    SampleBindingModel model;
    model.Begin(42, 3, 100);
    ASSERT_TRUE(model.NeedSend(100));
    model.Sent(false, 100);
    EXPECT_FALSE(model.NeedSend(199));
    EXPECT_TRUE(model.NeedSend(200));
    model.Sent(true, 200);
    EXPECT_FALSE(model.NeedSend(999));
    ASSERT_TRUE(model.NeedRead(200));
    model.Requested(123, true, 200);
    TrackStateMessage reply;
    reply.request_id = 122;
    reply.track = 3;
    reply.valid = reply.loaded = 1;
    reply.sample_id = 42;
    EXPECT_FALSE(model.Accept(reply));
    reply.request_id = 123;
    reply.track = 2;
    EXPECT_FALSE(model.Accept(reply));
    reply.track = 3;
    reply.busy = 1;
    EXPECT_FALSE(model.Accept(reply));
    EXPECT_TRUE(model.Active());
    EXPECT_FALSE(model.NeedSend(950));
    ASSERT_TRUE(model.NeedRead(950));
    model.Requested(124, true, 950);
    reply.request_id = 124;
    reply.busy = 0;
    EXPECT_TRUE(model.Accept(reply));
    EXPECT_EQ(model.Status(), SampleBindingModel::State::Confirmed);
}
TEST(SampleBindingModel, ReadLossRetriesOnlyReadsAndMismatchFails) {
    SampleBindingModel model;
    model.Begin(42, 3, 0);
    model.Sent(true, 0);
    model.Requested(1, false, 0);
    EXPECT_FALSE(model.NeedRead(100));
    EXPECT_TRUE(model.NeedRead(750));
    EXPECT_FALSE(model.NeedSend(750));
    model.Requested(2, true, 750);
    TrackStateMessage reply;
    reply.request_id = 2;
    reply.track = 3;
    reply.valid = reply.loaded = 1;
    reply.sample_id = 99;
    EXPECT_TRUE(model.Accept(reply));
    EXPECT_EQ(model.Status(), SampleBindingModel::State::Failed);
}
TEST(SampleBindingModel, TimeoutDisconnectAndNavigationCannotReplayBinding) {
    for (int failure = 0; failure < 3; ++failure) {
        SampleBindingModel model;
        model.Begin(42, 3, 10);
        if (failure == 0)
            model.Check(5010, true);
        if (failure == 1)
            model.Check(20, false);
        if (failure == 2)
            model.Cancel();
        model.Check(5020, true);
        EXPECT_FALSE(model.Active());
        EXPECT_FALSE(model.NeedSend(5020));
        EXPECT_FALSE(model.NeedRead(5020));
    }
}
