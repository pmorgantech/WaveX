#include "ui/track_page_model.h"

#include <gtest/gtest.h>
using namespace WaveX::Protocol;
using wavex_ui::TrackPageModel;
TEST(TrackPageModelTest, RejectsOldTrackOldReadAndInvalidRouting) {
    TrackPageModel model;
    model.Reset(15);
    model.Expect(100);
    TrackStateMessage s{};
    s.request_id = 100;
    s.track = 15;
    s.valid = 1;
    s.midi_in = 16;
    ASSERT_TRUE(model.Accept(s));
    ASSERT_TRUE(model.Ready());
    model.Reset(0);
    model.Expect(101);
    EXPECT_FALSE(model.Accept(s));
    EXPECT_FALSE(model.Ready());
    s.track = 0;
    EXPECT_FALSE(model.Accept(s));
    s.request_id = 101;
    s.midi_in = 17;
    EXPECT_FALSE(model.Accept(s));
    s.midi_in = 255;
    ASSERT_TRUE(model.Accept(s));
    EXPECT_EQ(model.State().midi_in, 255);
    s.busy = 1;
    ASSERT_TRUE(model.Accept(s));
    EXPECT_FALSE(model.Ready());
    s.busy = 2;
    EXPECT_FALSE(model.Accept(s));
    s.busy = 0;
    std::memset(s.name, 'x', sizeof(s.name));
    EXPECT_FALSE(model.Accept(s));
}
TEST(TrackPageModelTest, MidiChoicesIncludeOmniAndOffWithoutWrapping) {
    EXPECT_EQ(TrackPageModel::StepMidi(0, -1), 0);
    EXPECT_EQ(TrackPageModel::StepMidi(0, 1), 1);
    EXPECT_EQ(TrackPageModel::StepMidi(16, 1), 255);
    EXPECT_EQ(TrackPageModel::StepMidi(255, -1), 16);
    EXPECT_EQ(TrackPageModel::StepMidi(255, 1), 255);
}
