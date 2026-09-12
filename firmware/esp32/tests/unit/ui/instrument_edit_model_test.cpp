#include "ui/instrument_edit_model.h"

#include <gtest/gtest.h>

#include <limits>
using namespace WaveX::Protocol;
using wavex_ui::InstrumentEditModel;
TEST(InstrumentEditModel, FinalGainMotionAndBackendUndoAreSeparate) {
    InstrumentEditModel m;
    m.Reset(3);
    m.Expect(10);
    InstEditSyncMessage s;
    s.request_id = 10;
    s.revision = 7;
    s.track = 3;
    s.valid = 1;
    ASSERT_TRUE(m.Accept(s));
    ASSERT_TRUE(m.Set(2, 500));
    m.Sent(11, INST_EDIT_AMP);
    ASSERT_TRUE(m.Set(2, 0));
    s.request_id = s.completed_request_id = 11;
    s.revision++;
    s.dirty = 1;
    s.sound.gain = .5f;
    ASSERT_TRUE(m.Accept(s));
    EXPECT_TRUE(m.Outgoing());
    EXPECT_TRUE(m.Dirty());
    EXPECT_EQ(m.Value(2), 0);
    m.Sent(12, INST_EDIT_AMP);
    s.request_id = s.completed_request_id = 12;
    s.revision++;
    s.sound.gain = 0;
    ASSERT_TRUE(m.Accept(s));
    EXPECT_FALSE(m.Outgoing());
    EXPECT_TRUE(m.Dirty());
    m.Sent(13, INST_EDIT_APPLY);
    s.request_id = s.completed_request_id = 13;
    s.revision++;
    s.dirty = 0;
    ASSERT_TRUE(m.Accept(s));
    EXPECT_FALSE(m.Dirty());
}
TEST(InstrumentEditModel, KnownOwnRevisionAndFreshReadKeepOutgoingValue) {
    InstrumentEditModel m;
    m.Reset(0);
    m.Expect(10);
    InstEditSyncMessage s;
    s.request_id = 10;
    s.revision = 7;
    s.valid = 1;
    ASSERT_TRUE(m.Accept(s));
    m.AdoptRevision(8);
    ASSERT_TRUE(m.Set(2, 250));
    m.Expect(11);
    s.request_id = 11;
    EXPECT_FALSE(m.Accept(s));
    s.revision = 8;
    ASSERT_TRUE(m.Accept(s));
    EXPECT_TRUE(m.Outgoing());
    EXPECT_EQ(m.Value(2), 250);
    m.Expect(12);
    s.request_id = 12;
    EXPECT_TRUE(m.Accept(s));  // An action can wait for this fresh GET identity.
}

TEST(InstrumentEditModel, RejectsNonfiniteReadbackAndPreservesStoredCutoffOnOtherEdits) {
    InstrumentEditModel m;
    m.Reset(0);
    m.Expect(10);
    InstEditSyncMessage s;
    s.request_id = 10;
    s.revision = 7;
    s.valid = 1;
    s.sound.cutoff_hz = std::numeric_limits<float>::quiet_NaN();
    EXPECT_FALSE(m.Accept(s));
    s.sound.cutoff_hz = 96000;
    ASSERT_TRUE(m.Accept(s));
    ASSERT_TRUE(m.Set(1, 25000));
    auto r = m.Request(11, INST_EDIT_FILTER);
    EXPECT_FLOAT_EQ(r.sound.cutoff_hz, 96000);
    EXPECT_TRUE(IsValidInstEditOp(r));
}
