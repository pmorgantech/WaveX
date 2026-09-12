
#include "ui/modulator_model.h"

#include <gtest/gtest.h>

#include <limits>
using namespace WaveX::Protocol;
using wavex_ui::ModulatorModel;
namespace {
InstModSyncMessage snapshot() {
    InstModSyncMessage s;
    s.track = 3;
    s.request_id = 10;
    s.revision = 7;
    s.valid = 1;
    s.envelopes[2].attack_s = .123456f;
    s.slots[7].source = 16;
    s.slots[7].destination = 3;
    s.slots[7].depth = -12000;
    return s;
}
ModulatorModel model() {
    ModulatorModel m;
    m.Reset(3);
    m.Expect(10);
    EXPECT_TRUE(m.Accept(snapshot()));
    return m;
}
}  // namespace
TEST(ModulatorModel, IdentityAndNonfiniteEnvelopeCannotReplaceState) {
    auto m = model();
    auto s = snapshot();
    s.request_id++;
    EXPECT_FALSE(m.Accept(s));
    s = snapshot();
    s.track++;
    EXPECT_FALSE(m.Accept(s));
    s = snapshot();
    s.envelopes[2].sustain = std::numeric_limits<float>::quiet_NaN();
    EXPECT_FALSE(m.Accept(s));
    EXPECT_EQ(m.Snapshot().revision, 7u);
}
TEST(ModulatorModel, DraftSurvivesPollingButExternalRevisionDiscardsIt) {
    auto m = model();
    ASSERT_TRUE(m.Select(true, 2));
    ASSERT_TRUE(m.Set(3, 600000));
    EXPECT_FALSE(m.Select(false, 7));
    auto r = m.Request(11);
    EXPECT_FLOAT_EQ(r.envelope.attack_s, .123456f);
    EXPECT_FLOAT_EQ(r.envelope.release_s, 600);
    EXPECT_TRUE(IsValidInstModOp(r));
    m.Expect(12);
    auto s = snapshot();
    s.request_id = 12;
    EXPECT_FALSE(m.Accept(s));
    EXPECT_TRUE(m.Dirty());
    s.revision++;
    EXPECT_TRUE(m.Accept(s));
    EXPECT_FALSE(m.Dirty());
    EXPECT_TRUE(m.Conflict());
}
TEST(ModulatorModel, PendingMutationRequiresRetainedCompletionAndRejectionUsesReadback) {
    auto m = model();
    ASSERT_TRUE(m.Select(false, 7));
    ASSERT_TRUE(m.Set(2, 32767));
    m.MutationSent(11);
    EXPECT_FALSE(m.Set(0, 3));
    EXPECT_FALSE(m.Select(true, 0));
    m.Expect(12);
    auto s = snapshot();
    s.request_id = 12;
    s.busy = 1;
    ASSERT_TRUE(m.Accept(s));
    EXPECT_TRUE(m.Pending());
    s.busy = 0;
    s.completed_request_id = 11;
    s.error = INST_ERROR_BAD_FILE;
    ASSERT_TRUE(m.Accept(s));
    EXPECT_FALSE(m.Pending());
    EXPECT_FALSE(m.Dirty());
    EXPECT_EQ(m.Value(2), -12000);
    ASSERT_TRUE(m.Set(2, -32767));
    auto r = m.Request(13);
    EXPECT_EQ(r.index, 7);
    EXPECT_EQ(r.op, INST_MOD_SET_SLOT);
    EXPECT_TRUE(IsValidInstModOp(r));
}
TEST(ModulatorModel, UnknownRoutesRemainReadableAndCanBeExplicitlyCleared) {
    auto m = model();
    auto s = snapshot();
    s.revision++;
    s.slots[7].source = 200;
    ASSERT_TRUE(m.Accept(s));
    ASSERT_TRUE(m.Select(true, 2));
    EXPECT_TRUE(m.Set(2, 450));
    EXPECT_TRUE(IsValidInstModOp(m.Request(11)));
    m.Revert();
    ASSERT_TRUE(m.Select(false, 7));
    EXPECT_EQ(m.Value(0), 200);
    EXPECT_FALSE(IsValidInstModOp(m.Request(12)));
    EXPECT_TRUE(m.Clear());
    EXPECT_TRUE(IsValidInstModOp(m.Request(13)));
    m.Revert();
    EXPECT_EQ(m.Value(0), 200);
}
