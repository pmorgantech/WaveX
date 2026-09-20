#include "ui/allocation_model.h"

#include <gtest/gtest.h>
using namespace WaveX::Protocol;
using wavex_ui::AllocationModel;
TEST(AllocationModel, CorrelatesReadsEditsAndRetainedCompletions) {
    AllocationModel m;
    m.Reset(3, ALLOC_TRACK);
    m.Expect(9);
    AllocationSyncMessage state;
    state.request_id = 9;
    state.track = 3;
    state.scope = ALLOC_TRACK;
    state.valid = 1;
    state.revision = 2;
    ASSERT_TRUE(m.Accept(state));
    ASSERT_TRUE(m.Ready());
    AllocationOpMessage request;
    ASSERT_TRUE(m.Begin(10, ALLOC_SET, {}, false, request));
    EXPECT_FALSE(m.Ready());
    EXPECT_EQ(request.revision, 2u);
    m.Expect(11);
    state.request_id = 11;
    ASSERT_TRUE(m.Accept(state));
    EXPECT_TRUE(m.Pending());
    m.Expect(12);
    state.request_id = 12;
    state.revision = 3;
    state.completed_request_id = 10;
    ASSERT_TRUE(m.Accept(state));
    EXPECT_FALSE(m.Pending());
    EXPECT_TRUE(m.Ready());
    state.request_id = 10;
    state.revision = 2;
    EXPECT_FALSE(m.Accept(state));
}
TEST(AllocationModel, RejectsOtherScopeTrackMalformedAndStaleSnapshots) {
    AllocationModel m;
    m.Reset(1, ALLOC_SOUND);
    m.Expect(8);
    AllocationSyncMessage s;
    s.request_id = 8;
    s.track = 1;
    s.valid = 1;
    s.revision = 5;
    auto bad = s;
    bad.scope = ALLOC_TRACK;
    EXPECT_FALSE(m.Accept(bad));
    bad = s;
    bad.track = 2;
    EXPECT_FALSE(m.Accept(bad));
    bad = s;
    bad.sound.limit = 9;
    EXPECT_FALSE(m.Accept(bad));
    ASSERT_TRUE(m.Accept(s));
    m.Expect(9);
    s.request_id = 9;
    s.revision = 4;
    EXPECT_FALSE(m.Accept(s));
    m.Reset(2, ALLOC_SOUND);
    EXPECT_FALSE(m.Accept(s));
    EXPECT_FALSE(m.Ready());
}
TEST(AllocationModel, EffectivePolicyRespectsInheritanceAndCannotInheritAtSoundScope) {
    AllocationModel m;
    m.Reset(0, ALLOC_TRACK);
    m.Expect(1);
    AllocationSyncMessage s;
    s.request_id = 1;
    s.scope = ALLOC_TRACK;
    s.valid = 1;
    s.revision = 1;
    s.sound.mode = WaveX::Allocation::PlayMode::Mono;
    s.track_policy.limit = 3;
    ASSERT_TRUE(m.Accept(s));
    EXPECT_EQ(m.Effective(), s.sound);
    m.Expect(2);
    s.request_id = 2;
    s.inherited = 0;
    ASSERT_TRUE(m.Accept(s));
    EXPECT_EQ(m.Effective(), s.track_policy);
    m.Reset(0, ALLOC_SOUND);
    m.Expect(3);
    s.request_id = 3;
    s.scope = ALLOC_SOUND;
    ASSERT_TRUE(m.Accept(s));
    AllocationOpMessage out;
    EXPECT_FALSE(m.Begin(4, ALLOC_SET, {}, true, out));
}
