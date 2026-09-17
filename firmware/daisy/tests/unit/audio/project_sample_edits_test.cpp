#include "audio/project_sample_edits.hpp"

#include <gtest/gtest.h>

using namespace WaveX;
using namespace WaveX::AudioEngine;
TEST(ProjectSampleEdits, RejectsChangedDependencyWithoutChangingCandidateMetadata) {
    LoadedSampleInfo source;
    ResidentSampleInfo resident{};
    resident.sample_rate = 48000;
    resident.total_frames = 48000;
    resident.channels = 2;
    resident.bit_depth = 16;
    FillLoadedSample(source, 1024, "/sample.wav", resident, {});
    source.meta.start_frame = 100;
    source.meta.loop_start = 500;
    source.meta.gain_db_x10 = -60;
    source.meta.fade_in_ms = 15;
    source.meta.fade_out_ms = 20;
    source.meta.loop_enabled = 1;
    source.meta.channel_mode = Protocol::SAMPLE_CH_LEFT;
    Sequencer::ProjectSample saved;
    ASSERT_TRUE(CaptureProjectSample(source, saved));
    LoadedSampleInfo candidate;
    FillLoadedSample(candidate, 2048, "/sample.wav", resident, {});
    candidate.meta.total_frames = 24000;
    EXPECT_FALSE(ApplyProjectSample(saved, candidate));
    EXPECT_EQ(candidate.meta.start_frame, 0u);
    candidate.meta.total_frames = resident.total_frames;
    ASSERT_TRUE(ApplyProjectSample(saved, candidate));
    EXPECT_EQ(candidate.meta.sample_id, 2048);
    EXPECT_EQ(candidate.meta.start_frame, 100u);
    EXPECT_EQ(candidate.meta.loop_start, 500u);
    EXPECT_EQ(candidate.meta.gain_db_x10, -60);
    EXPECT_EQ(candidate.meta.fade_in_ms, 15);
    EXPECT_EQ(candidate.meta.fade_out_ms, 20);
    EXPECT_EQ(candidate.meta.channel_mode, Protocol::SAMPLE_CH_LEFT);
    EXPECT_EQ(candidate.meta.flags, Protocol::SAMPLE_META_RESIDENT);
}
