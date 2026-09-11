#include "ui/pad_sound_model.h"

#include <gtest/gtest.h>
using namespace WaveX::Protocol;
using wavex_ui::PadSoundModel;

TEST(PadSoundModel, RapidEditsSurviveOlderReadbackAndSerializeFields) {
    PadSoundModel model;
    model.Reset(2, 15);
    model.Expect(1);
    InstPadSoundSyncMessage state{1, 0, 2, 15, 1, 0, 0, 0, 23, 20000, 1, 50, 800};
    ASSERT_TRUE(model.Accept(state));
    ASSERT_TRUE(model.Set(0, 3000));
    InstPadSoundOpMessage request;
    ASSERT_TRUE(model.Next(2, request));
    EXPECT_EQ(request.sample_id, 23);
    EXPECT_EQ(request.value, 3000);
    ASSERT_TRUE(model.Set(0, 700));
    ASSERT_TRUE(model.Set(2, 200));
    model.Expect(3);
    state.request_id = 3;
    state.completed_request_id = 2;
    state.cutoff_hz = 3000;
    state.own = 1;
    ASSERT_TRUE(model.Accept(state));
    EXPECT_EQ(model.Value(0), 700);
    ASSERT_TRUE(model.Next(4, request));
    EXPECT_EQ(request.value, 700);
    EXPECT_EQ(request.op, PAD_SOUND_CUTOFF);
    state.request_id = 4;
    state.completed_request_id = 4;
    state.cutoff_hz = 700;
    ASSERT_TRUE(model.Accept(state));
    ASSERT_TRUE(model.Next(5, request));
    EXPECT_EQ(request.op, PAD_SOUND_DECAY);
    state.request_id = 5;
    state.completed_request_id = 5;
    state.decay_ms = 200;
    ASSERT_TRUE(model.Accept(state));
    EXPECT_TRUE(model.Ready());
    EXPECT_EQ(model.Value(0), 700);
    EXPECT_EQ(model.Value(2), 200);
}
TEST(PadSoundModel, WrongIdentityDoesNotUnlockPendingAndFailureRestoresReadback) {
    PadSoundModel model;
    model.Reset(0, 0);
    model.Expect(1);
    InstPadSoundSyncMessage s{1, 0, 0, 0, 1, 0, 0, 0, 10, 20000, 1, 50, 800};
    ASSERT_TRUE(model.Accept(s));
    ASSERT_TRUE(model.Set(1, 250));
    InstPadSoundOpMessage request;
    ASSERT_TRUE(model.Next(2, request));
    s.request_id = 1;
    s.completed_request_id = 2;
    EXPECT_FALSE(model.Accept(s));
    s.request_id = 2;
    s.pad = 1;
    EXPECT_FALSE(model.Accept(s));
    EXPECT_TRUE(model.Pending());
    s.pad = 0;
    s.error = INST_ERROR_BUSY;
    ASSERT_TRUE(model.Accept(s));
    EXPECT_FALSE(model.Pending());
    EXPECT_EQ(model.Value(1), 1);
    EXPECT_EQ(model.Error(), INST_ERROR_BUSY);
}
TEST(PadSoundModel, InheritWaitsForAcknowledgementAndSelectionChangeDropsLocalEdits) {
    PadSoundModel model;
    model.Reset(0, 0);
    model.Expect(1);
    InstPadSoundSyncMessage s{1, 0, 0, 0, 1, 0, 0, 1, 10, 700, 1, 50, 800};
    ASSERT_TRUE(model.Accept(s));
    ASSERT_TRUE(model.Inherit());
    EXPECT_FALSE(model.Set(0, 900));
    InstPadSoundOpMessage request;
    ASSERT_TRUE(model.Next(2, request));
    EXPECT_EQ(request.op, PAD_SOUND_INHERIT);
    s.request_id = 2;
    s.completed_request_id = 2;
    s.own = 0;
    s.cutoff_hz = 20000;
    ASSERT_TRUE(model.Accept(s));
    EXPECT_TRUE(model.Ready());
    ASSERT_TRUE(model.Set(0, 900));
    ASSERT_TRUE(model.Next(3, request));
    s.request_id = 3;
    s.sample_id = 99;
    ASSERT_TRUE(model.Accept(s));
    EXPECT_FALSE(model.Pending());
    EXPECT_EQ(model.Value(0), 20000);
    model.Reset(1, 0);
    EXPECT_FALSE(model.Accept(s));
    EXPECT_FALSE(model.Valid());
}
