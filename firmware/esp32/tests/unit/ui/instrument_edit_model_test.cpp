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

TEST(InstrumentEditModel, FilterModeAndCutoffCoalesceAndExternalReplacementWins) {
    InstrumentEditModel m;
    m.Reset(0);
    m.Expect(10);
    InstEditSyncMessage s;
    s.request_id = 10;
    s.revision = 7;
    s.valid = 1;
    ASSERT_TRUE(m.Accept(s));
    ASSERT_TRUE(m.Set(4, INST_FILTER_HP));
    auto r = m.Request(11, m.Operation());
    EXPECT_EQ(r.op, INST_EDIT_FILTER_SETTINGS);
    EXPECT_EQ(r.filter_type, INST_FILTER_HP);
    m.Sent(11, r.op);
    ASSERT_TRUE(m.Set(0, 20000));
    ASSERT_TRUE(m.Set(4, INST_FILTER_NOTCH));
    s.request_id = s.completed_request_id = 11;
    ++s.revision;
    s.filter_type = INST_FILTER_HP;
    ASSERT_TRUE(m.Accept(s));
    ASSERT_TRUE(m.Outgoing());
    r = m.Request(12, m.Operation());
    EXPECT_EQ(r.filter_type, INST_FILTER_NOTCH);
    EXPECT_NE(r.sound.cutoff_hz, s.sound.cutoff_hz);
    ASSERT_TRUE(IsValidInstEditOp(r));
    m.Sent(12, r.op);
    s.request_id = s.completed_request_id = 12;
    ++s.revision;
    s.filter_type = r.filter_type;
    s.sound = r.sound;
    ASSERT_TRUE(m.Accept(s));
    EXPECT_FALSE(m.Outgoing());
    m.Expect(13);
    s.request_id = 13;
    ++s.revision;
    s.filter_type = INST_FILTER_BP;
    ASSERT_TRUE(m.Accept(s));
    EXPECT_EQ(m.Value(4), INST_FILTER_BP);
    m.Expect(14);
    s.request_id = 14;
    s.filter_type = 4;
    EXPECT_FALSE(m.Accept(s));
}

TEST(InstrumentEditModel, FilterTopologyTravelsWithTheSettingsOpAndCoalesces) {
    InstrumentEditModel m;
    m.Reset(0);
    m.Expect(10);
    InstEditSyncMessage s;
    s.request_id = 10;
    s.revision = 7;
    s.valid = 1;
    ASSERT_TRUE(m.Accept(s));
    EXPECT_EQ(m.Value(5), INST_FILTER_TOPOLOGY_SVF);
    EXPECT_FALSE(m.Set(5, INST_FILTER_TOPOLOGY_COUNT));
    ASSERT_TRUE(m.Set(5, INST_FILTER_TOPOLOGY_LADDER));
    EXPECT_TRUE(m.Outgoing());
    auto r = m.Request(11, m.Operation());
    EXPECT_EQ(r.op, INST_EDIT_FILTER_SETTINGS);
    EXPECT_EQ(r.filter_topology, INST_FILTER_TOPOLOGY_LADDER);
    EXPECT_EQ(r.filter_type, INST_FILTER_LP);
    ASSERT_TRUE(IsValidInstEditOp(r));
    // Any other op leaves the byte zero, as the validator requires.
    EXPECT_EQ(m.Request(11, INST_EDIT_AMP).filter_topology, 0);
    m.Sent(11, r.op);
    ASSERT_TRUE(m.Set(5, INST_FILTER_TOPOLOGY_SVF));  // changed again while in flight
    s.request_id = s.completed_request_id = 11;
    ++s.revision;
    s.filter_topology = INST_FILTER_TOPOLOGY_LADDER;
    ASSERT_TRUE(m.Accept(s));
    EXPECT_TRUE(m.Outgoing());
    r = m.Request(12, m.Operation());
    EXPECT_EQ(r.filter_topology, INST_FILTER_TOPOLOGY_SVF);
    m.Sent(12, r.op);
    s.request_id = s.completed_request_id = 12;
    ++s.revision;
    s.filter_topology = r.filter_topology;
    ASSERT_TRUE(m.Accept(s));
    EXPECT_FALSE(m.Outgoing());
    // A backend that reports a topology this build does not know is not
    // adopted, exactly like an unknown mode.
    m.Expect(13);
    s.request_id = 13;
    ++s.revision;
    s.filter_topology = INST_FILTER_TOPOLOGY_COUNT;
    EXPECT_FALSE(m.Accept(s));
    s.filter_topology = INST_FILTER_TOPOLOGY_LADDER;
    ASSERT_TRUE(m.Accept(s));
    EXPECT_EQ(m.Value(5), INST_FILTER_TOPOLOGY_LADDER);
}

TEST(InstrumentEditModel, SlopeAndDriveAreFilterSettingsFieldsInThousandths) {
    InstrumentEditModel m;
    m.Reset(0);
    m.Expect(10);
    InstEditSyncMessage s;
    s.request_id = 10;
    s.revision = 7;
    s.valid = 1;
    ASSERT_TRUE(m.Accept(s));
    EXPECT_EQ(m.Value(6), INST_FILTER_SLOPE_12);
    EXPECT_EQ(m.Value(7), 0);
    EXPECT_FALSE(m.Set(6, 2));
    EXPECT_FALSE(m.Set(7, 1001));
    ASSERT_TRUE(m.Set(6, INST_FILTER_SLOPE_24));
    ASSERT_TRUE(m.Set(7, 650));
    EXPECT_TRUE(m.Outgoing());
    auto r = m.Request(11, m.Operation());
    EXPECT_EQ(r.op, INST_EDIT_FILTER_SETTINGS);
    EXPECT_EQ(r.filter_slope, INST_FILTER_SLOPE_24);
    EXPECT_FLOAT_EQ(r.filter_drive, .65f);
    ASSERT_TRUE(IsValidInstEditOp(r));
    const auto amp = m.Request(11, INST_EDIT_AMP);
    EXPECT_EQ(amp.filter_slope, 0);
    EXPECT_FLOAT_EQ(amp.filter_drive, 0);
    m.Sent(11, r.op);
    s.request_id = s.completed_request_id = 11;
    ++s.revision;
    s.filter_slope = r.filter_slope;
    s.filter_drive = r.filter_drive;
    ASSERT_TRUE(m.Accept(s));
    EXPECT_FALSE(m.Outgoing());
    EXPECT_EQ(m.Value(7), 650);
    // A readback the frontend cannot represent is refused like a bad mode.
    m.Expect(12);
    s.request_id = 12;
    ++s.revision;
    s.filter_drive = 1.5f;
    EXPECT_FALSE(m.Accept(s));
    s.filter_drive = 1.0f;
    s.reserved_tail[2] = 1;
    EXPECT_FALSE(m.Accept(s));
    s.reserved_tail[2] = 0;
    s.filter_slope = 2;
    EXPECT_FALSE(m.Accept(s));
    s.filter_slope = INST_FILTER_SLOPE_12;
    ASSERT_TRUE(m.Accept(s));
    EXPECT_EQ(m.Value(6), INST_FILTER_SLOPE_12);
    EXPECT_EQ(m.Value(7), 1000);
}
