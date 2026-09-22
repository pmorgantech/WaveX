#include "ui/instrument_tags_model.h"

#include <gtest/gtest.h>
using namespace WaveX::Protocol;
TEST(InstrumentTagsModel, StagesMultipleTagsAndRejectsStaleTrackAndRevision) {
    wavex_ui::InstrumentTagsModel m;
    m.Reset(2);
    m.Expect(10);
    InstZoneSyncMessage state;
    state.track = 2;
    state.request_id = 9;
    state.loaded = 1;
    state.revision = 7;
    state.tags = 4;
    EXPECT_FALSE(m.Accept(state));
    state.request_id = 10;
    m.Expect(10);
    ASSERT_TRUE(m.Accept(state));
    m.Toggle(0);
    m.Toggle(7);
    EXPECT_EQ(m.Draft(), 0x85);
    ASSERT_TRUE(m.Dirty());
    const auto edit = m.Request(20);
    EXPECT_EQ(edit.tags, 0x85);
    EXPECT_EQ(edit.revision, 7);
    EXPECT_EQ(edit.slot, 2);
    EXPECT_EQ(edit.op, INST_OP_SET_TAGS);
    m.Sent(20);
    m.Toggle(1);
    EXPECT_EQ(m.Draft(), 0x85);
    state.completed_request_id = 19;
    m.Expect(10);
    m.Accept(state);
    EXPECT_TRUE(m.Pending());
    state.completed_request_id = 20;
    state.tags = 0x85;
    state.revision = 8;
    m.Expect(10);
    ASSERT_TRUE(m.Accept(state));
    EXPECT_FALSE(m.Pending());
    EXPECT_FALSE(m.Dirty());
    m.Toggle(1);
    state.revision = 9;
    state.tags = 0;
    m.Expect(10);
    ASSERT_TRUE(m.Accept(state));
    EXPECT_FALSE(m.Dirty());
    EXPECT_EQ(m.Draft(), 0);
    state.track = 3;
    EXPECT_FALSE(m.Accept(state));
    m.Reset(2);
    EXPECT_FALSE(m.Ready());
}
