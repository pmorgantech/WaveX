#include "ui/arp_model.h"

#include <gtest/gtest.h>
using namespace wavex_ui;
using namespace WaveX::Protocol;
TEST(ArpModelTest, CorrelationPendingAndStaleRevisionsProtectWholeConfig) {
    ArpModel model;
    model.Reset(3);
    model.Expect(1);
    InstArpSyncMessage state;
    state.request_id = 1;
    state.track = 3;
    state.valid = 1;
    state.revision = 7;
    ASSERT_TRUE(model.Accept(state));
    ASSERT_TRUE(model.Set(0, 1));
    model.Expect(2);
    state.request_id = 2;
    model.Accept(state);
    EXPECT_TRUE(model.Dirty());
    EXPECT_EQ(model.Value(0), 1);
    const auto request = model.Request(3);
    model.Sent(3);
    EXPECT_TRUE(model.Set(1, 2));
    EXPECT_TRUE(model.Pending());
    state.request_id = 3;
    state.completed_request_id = 3;
    state.revision = 8;
    state.value = request.value;
    ASSERT_TRUE(model.Accept(state));
    EXPECT_FALSE(model.Pending());
    EXPECT_TRUE(model.Dirty());
    EXPECT_EQ(model.Value(1), 2);
    state.revision = 7;
    state.value.enabled = 0;
    EXPECT_FALSE(model.Accept(state));
    EXPECT_EQ(model.Value(0), 1);
}
TEST(ArpModelTest, ResetDiscardsUnconfirmedEditsWithoutReplayingThem) {
    ArpModel model;
    model.Reset(0);
    model.Expect(1);
    InstArpSyncMessage state;
    state.request_id = 1;
    state.revision = 1;
    state.valid = 1;
    ASSERT_TRUE(model.Accept(state));
    ASSERT_TRUE(model.Set(1, 5));
    model.Sent(2);
    model.Reset(0);
    EXPECT_FALSE(model.Pending());
    EXPECT_FALSE(model.Dirty());
    EXPECT_FALSE(model.Ready());
    EXPECT_FALSE(model.Accept(state));
}
