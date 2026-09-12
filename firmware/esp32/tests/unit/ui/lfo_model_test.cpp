#include "ui/lfo_model.h"

#include <gtest/gtest.h>

#include <limits>
using namespace WaveX::Protocol;
using wavex_ui::LfoModel;
TEST(LfoModel, RapidPreviewKeepsLatestValueAndOtherLfo) {
    LfoModel m;
    m.Reset(3);
    m.Expect(10);
    InstLfoSyncMessage s;
    s.request_id = 10;
    s.track = 3;
    s.revision = 7;
    s.valid = 1;
    s.values[1].fade_s = .25f;
    ASSERT_TRUE(m.Accept(s));
    ASSERT_TRUE(m.Set(1, 2500));
    m.Sent(11);
    ASSERT_TRUE(m.Set(1, 1000));
    EXPECT_FALSE(m.Select(1));
    s.request_id = s.completed_request_id = 11;
    ++s.revision;
    s.values[0].rate_hz = 2.5f;
    ASSERT_TRUE(m.Accept(s));
    EXPECT_TRUE(m.Dirty());
    EXPECT_EQ(m.Value(1), 1000);
    m.Sent(12);
    s.request_id = s.completed_request_id = 12;
    ++s.revision;
    s.values[0].rate_hz = 1;
    ASSERT_TRUE(m.Accept(s));
    EXPECT_FALSE(m.Dirty());
    ASSERT_TRUE(m.Select(1));
    EXPECT_EQ(m.Value(5), 250);
    EXPECT_TRUE(IsValidInstLfoOp(m.Request(13)));
}
TEST(LfoModel, RejectsWrongIdentityAndNonfiniteReadback) {
    LfoModel m;
    m.Reset(3);
    m.Expect(10);
    InstLfoSyncMessage s;
    s.request_id = 9;
    s.track = 3;
    s.revision = 7;
    s.valid = 1;
    EXPECT_FALSE(m.Accept(s));
    s.request_id = 10;
    s.values[0].delay_s = std::numeric_limits<float>::quiet_NaN();
    EXPECT_FALSE(m.Accept(s));
    s.values[0].delay_s = 0;
    ASSERT_TRUE(m.Accept(s));
    ASSERT_TRUE(m.Set(0, 4));
    m.AdoptRevision(8);
    EXPECT_FALSE(m.Accept(s));
    s.revision = 8;
    EXPECT_FALSE(m.Accept(s));
    EXPECT_TRUE(m.Dirty());
    EXPECT_FALSE(m.Set(1, 0));
    EXPECT_FALSE(m.Set(4, 600001));
    EXPECT_FALSE(m.Set(6, 2));
}
TEST(LfoModel, RejectionAndExternalReplacementRestoreAuthoritativeSettings) {
    LfoModel m;
    m.Reset(0);
    m.Expect(10);
    InstLfoSyncMessage s;
    s.request_id = 10;
    s.revision = 7;
    s.valid = 1;
    s.values[0].rate_hz = 1000;
    s.values[0].wave = 99;
    ASSERT_TRUE(m.Accept(s));
    ASSERT_TRUE(m.Set(0, 2));
    EXPECT_FLOAT_EQ(m.Request(11).value.rate_hz, 1000);
    m.Sent(11);
    s.request_id = s.completed_request_id = 11;
    s.error = 1;
    ASSERT_TRUE(m.Accept(s));
    EXPECT_FALSE(m.Dirty());
    EXPECT_EQ(m.Value(0), 99);
    ASSERT_TRUE(m.Set(0, 1));
    m.Expect(12);
    s.request_id = 12;
    s.revision++;
    s.values[0].wave = 3;
    ASSERT_TRUE(m.Accept(s));
    EXPECT_FALSE(m.Dirty());
    EXPECT_EQ(m.Value(0), 3);
}
