#include "ui/oscillator_model.h"

#include <gtest/gtest.h>

#include <limits>
using namespace WaveX::Protocol;
using wavex_ui::OscillatorModel;
namespace {
InstOscSyncMessage snapshot() {
    InstOscSyncMessage s;
    s.request_id = 10;
    s.revision = 3;
    s.track = 2;
    s.oscillator = 1;
    s.valid = 1;
    s.type = 1;
    s.zones = 4;
    return s;
}
OscillatorModel model() {
    OscillatorModel m;
    m.Reset(2, 1);
    m.Expect(10);
    EXPECT_TRUE(m.Accept(snapshot()));
    return m;
}
}  // namespace
TEST(OscillatorModel, RejectsStaleIdentityAndInvalidSnapshot) {
    auto m = model();
    auto s = snapshot();
    s.request_id = 9;
    EXPECT_FALSE(m.Accept(s));
    s = snapshot();
    s.track = 3;
    EXPECT_FALSE(m.Accept(s));
    s = snapshot();
    s.oscillator = 0;
    EXPECT_FALSE(m.Accept(s));
    s = snapshot();
    s.value.level = std::numeric_limits<float>::quiet_NaN();
    EXPECT_FALSE(m.Accept(s));
    EXPECT_EQ(m.Snapshot().revision, 3u);
}
TEST(OscillatorModel, DraftSurvivesReadsButNotReplacement) {
    auto m = model();
    ASSERT_TRUE(m.Set(2, 17));
    auto s = snapshot();
    s.request_id = 11;
    m.Expect(11);
    ASSERT_TRUE(m.Accept(s));
    EXPECT_TRUE(m.Dirty());
    EXPECT_EQ(m.Value(2), 17);
    s.revision++;
    ASSERT_TRUE(m.Accept(s));
    EXPECT_FALSE(m.Dirty());
    EXPECT_TRUE(m.Conflict());
    EXPECT_EQ(m.Value(2), 0);
}
TEST(OscillatorModel, AcknowledgementAndRejectionRestoreAuthoritativeValues) {
    auto m = model();
    ASSERT_TRUE(m.Set(1, 500));
    const auto request = m.Request(12, INST_OSC_SET);
    EXPECT_EQ(request.revision, 3u);
    EXPECT_FLOAT_EQ(request.value.mix, .5f);
    m.MutationSent(12);
    EXPECT_FALSE(m.Ready());
    auto s = snapshot();
    s.request_id = s.completed_request_id = 12;
    s.revision++;
    s.value.mix = .5f;
    EXPECT_TRUE(m.Accept(s));
    EXPECT_FALSE(m.Pending());
    EXPECT_FALSE(m.Dirty());
    EXPECT_FALSE(m.Conflict());
    EXPECT_EQ(m.Value(1), 500);
    m.Set(1, 250);
    m.MutationSent(13);
    s.request_id = s.completed_request_id = 13;
    s.error = INST_ERROR_BAD_FILE;
    EXPECT_TRUE(m.Accept(s));
    EXPECT_FALSE(m.Pending());
    EXPECT_EQ(m.Value(1), 500);
}
TEST(OscillatorModel, PreservesUntouchedFloatsAndFullStoredRanges) {
    auto m = model();
    auto s = snapshot();
    s.value.level = 63.12345f;
    s.value.mix = .123456f;
    s.revision++;
    ASSERT_TRUE(m.Accept(s));
    ASSERT_TRUE(m.Set(2, -128));
    ASSERT_TRUE(m.Set(3, 127));
    auto r = m.Request(12, INST_OSC_SET);
    EXPECT_FLOAT_EQ(r.value.level, s.value.level);
    EXPECT_FLOAT_EQ(r.value.mix, s.value.mix);
    EXPECT_EQ(r.value.coarse, -128);
    EXPECT_EQ(r.value.fine, 127);
    EXPECT_TRUE(IsValidInstOscOp(r));
    m.Revert();
    EXPECT_FALSE(m.Dirty());
    r = m.Request(13, INST_OSC_COPY_EMPTY);
    EXPECT_EQ(r.oscillator, 1);
    EXPECT_EQ(r.source, 0);
}

TEST(OscillatorModel, CoalescesFinalMotionIncludingReturningToOriginalValue) {
    auto m = model();
    ASSERT_TRUE(m.Set(1, 500));
    m.MutationSent(12);
    ASSERT_TRUE(m.Set(1, 0));
    auto s = snapshot();
    s.request_id = s.completed_request_id = 12;
    s.revision++;
    s.value.mix = .5f;
    ASSERT_TRUE(m.Accept(s));
    EXPECT_TRUE(m.Ready());
    EXPECT_TRUE(m.Dirty());
    EXPECT_EQ(m.Value(1), 0);
    m.MutationSent(13);
    s.request_id = s.completed_request_id = 13;
    s.revision++;
    s.value.mix = 0;
    ASSERT_TRUE(m.Accept(s));
    EXPECT_FALSE(m.Dirty());
    auto stale = s;
    stale.revision--;
    EXPECT_FALSE(m.Accept(stale));
}

TEST(OscillatorModel, MonoRoundTripAndUndoKeepAuthoritativeState) {
    auto m = model();
    EXPECT_EQ(m.Value(5), 0);
    ASSERT_TRUE(m.Set(5, 1));
    const auto request = m.Request(12, INST_OSC_SET);
    EXPECT_EQ(request.value.mono, 1);
    m.MutationSent(12);
    auto s = snapshot();
    s.request_id = s.completed_request_id = 12;
    s.revision++;
    s.value.mono = 1;
    ASSERT_TRUE(m.Accept(s));
    EXPECT_FALSE(m.Dirty());
    ASSERT_TRUE(m.Set(5, 0));
    m.Revert();
    EXPECT_EQ(m.Value(5), 1);
    s.value.mono = 2;
    EXPECT_FALSE(m.Accept(s));
}
