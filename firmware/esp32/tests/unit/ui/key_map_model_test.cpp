
#include "ui/key_map_model.h"

#include <gtest/gtest.h>
using namespace WaveX::Protocol;
using wavex_ui::KeyMapModel;
TEST(KeyMapModel, StagesInclusiveRangesAndRequiresApplyOrRevertBeforeSelection) {
    KeyMapModel m;
    m.Reset(15);
    m.Expect(1);
    InstKeyMapSyncMessage s;
    s.request_id = 1;
    s.revision = 4;
    s.track = 15;
    s.loaded = 1;
    s.zones[31] = {99, 20, 70, 1, 80, 60};
    ASSERT_TRUE(m.Accept(s));
    ASSERT_TRUE(m.Select(31));
    ASSERT_TRUE(m.Set(0, 75));
    EXPECT_EQ(m.Value(1), 75);
    ASSERT_TRUE(m.Set(3, 50));
    ASSERT_TRUE(m.Set(2, 65));
    EXPECT_EQ(m.Value(3), 65);
    EXPECT_FALSE(m.Select(0));
    auto request = m.Request(2, KEY_MAP_SET_RANGE);
    EXPECT_EQ(request.zone, 31);
    EXPECT_EQ(request.revision, 4);
    EXPECT_EQ(request.expected_sample, 99);
    ASSERT_TRUE(IsValidKeyMapOp(request));
    m.MutationSent(2);
    m.Expect(3);
    s.request_id = 3;
    s.completed_request_id = 2;
    s.revision = 5;
    s.zones[31] = request.value;
    ASSERT_TRUE(m.Accept(s));
    EXPECT_FALSE(m.Dirty());
    EXPECT_FALSE(m.Conflict());
    EXPECT_TRUE(m.Select(0));
}
TEST(KeyMapModel, ReplacementRevisionDropsStaleDraftEvenWithSameSample) {
    KeyMapModel m;
    m.Reset(0);
    m.Expect(1);
    InstKeyMapSyncMessage s;
    s.request_id = 1;
    s.revision = 1;
    s.loaded = 1;
    s.zones[0].sample_id = 9;
    ASSERT_TRUE(m.Accept(s));
    ASSERT_TRUE(m.Set(4, 48));
    s.revision = 2;
    s.zones[0].root_note = 72;
    ASSERT_TRUE(m.Accept(s));
    EXPECT_FALSE(m.Dirty());
    EXPECT_TRUE(m.Conflict());
    EXPECT_EQ(m.Value(4), 72);
    s.track = 1;
    EXPECT_FALSE(m.Accept(s));
    s.track = 0;
    s.zones[31].vel_lo = 0;
    EXPECT_FALSE(m.Accept(s));
}
TEST(KeyMapModel, EmptyAndDrumMapsCannotStageKeyboardEdits) {
    KeyMapModel m;
    m.Reset(0);
    m.Expect(1);
    InstKeyMapSyncMessage s;
    s.request_id = 1;
    s.revision = 1;
    ASSERT_TRUE(m.Accept(s));
    EXPECT_FALSE(m.Set(0, 10));
    s.loaded = 1;
    s.mode = 1;
    s.zones[0].sample_id = 9;
    ASSERT_TRUE(m.Accept(s));
    EXPECT_FALSE(m.Set(0, 10));
}
