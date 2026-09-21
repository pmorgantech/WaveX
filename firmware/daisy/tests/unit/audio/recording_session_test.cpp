#include "audio/recording_session.hpp"

#include <gtest/gtest.h>

#include "fatfs_mock.h"

#include <array>
#include <vector>

using namespace WaveX::AudioEngine;
using namespace WaveX::Protocol;
namespace {
RecordStatusMessage latest;
int Send(uint16_t type, const void* data, uint16_t bytes) {
    if (type == MSG_REC_STATUS && bytes == sizeof(latest))
        std::memcpy(&latest, data, bytes);
    return 0;
}
void Meta(const LoadedSampleInfo&) {}
struct RecordingSessionTest : testing::Test {
    std::vector<uint8_t> arena = std::vector<uint8_t>(4 * 1024 * 1024);
    std::vector<SamplePool::Record> records =
        std::vector<SamplePool::Record>(WAVEX_SAMPLE_POOL_CAPACITY);
    SamplePool pool{records.data()};
    SampleMemMgr memory;
    RecordingSession session;
    VoiceManager voices;
    uint32_t id = 1, time = 1;
    std::array<float, 48> left{}, right{}, out_l{}, out_r{};
    void SetUp() override {
        MockFatFS::Instance().Reset();
        latest = {};
        ASSERT_TRUE(memory.init(arena.data(), static_cast<uint32_t>(arena.size()), 4096));
        session.Init(&pool, &memory, Meta, Send);
        voices.Init(48000);
        left.fill(.25f);
        right.fill(-.5f);
        out_l.fill(.75f);
        out_r.fill(-.25f);
    }
    void Pump() { session.Pump(time++); }
    void Request(uint8_t op, uint8_t source = REC_CODEC_STEREO, const char* name = "Test take") {
        RecordOpMessage request;
        request.request_id = id++;
        request.take_id = latest.take_id;
        request.op = op;
        request.source = source;
        request.max_frames = 48000;
        request.preroll_ms = 0;
        std::strncpy(request.name, name, sizeof(request.name) - 1);
        session.Request(request, false);
        Pump();
    }
    void Block() {
        session.Commands(voices);
        session.Process(left.data(), right.data(), out_l.data(), out_r.data(), 48);
        Pump();
    }
    void Capture(uint8_t source = REC_CODEC_STEREO) {
        Request(REC_ARM, source);
        Block();
        Request(REC_START);
        Block();
        Request(REC_STOP);
        Block();
        ASSERT_EQ(latest.state, REC_READY);
        ASSERT_EQ(latest.frames, 48u);
    }
};
TEST_F(RecordingSessionTest, SourceSelectionAndTakeMemoryOwnership) {
    for (uint8_t source = 0; source < 4; ++source) {
        Capture(source);
        ASSERT_NE(latest.sample_id, 0u);
        auto* record = pool.Find(latest.sample_id);
        ASSERT_NE(record, nullptr);
        EXPECT_EQ(record->payload.path[0], 0);
        EXPECT_TRUE(session.Owns(latest.sample_id));
        void* pcm = nullptr;
        ASSERT_TRUE(memory.ptr(record->payload.handle, &pcm));
        auto* samples = static_cast<int16_t*>(pcm);
        EXPECT_EQ(samples[0],
                  source == REC_INTERNAL_MIX  ? 24576
                  : source == REC_CODEC_RIGHT ? -16384
                                              : 8192);
        EXPECT_EQ(record->payload.channels, RecordChannels(source));
        const auto sample = latest.sample_id;
        Request(REC_DISCARD);
        EXPECT_NE(pool.Find(sample), nullptr);  // No callback acknowledgement yet.
        Block();
        EXPECT_EQ(pool.Find(sample), nullptr);
        EXPECT_EQ(latest.state, REC_IDLE);
    }
}
TEST_F(RecordingSessionTest, SavePromotesSameIdAndDonePreservesSavedSample) {
    Capture();
    const auto sample = latest.sample_id;
    Request(REC_SAVE);
    for (unsigned i = 0; i < 30; ++i)
        Pump();
    ASSERT_EQ(latest.error, REC_OK);
    ASSERT_EQ(latest.state, REC_READY);
    ASSERT_NE(latest.path[0], 0);
    EXPECT_EQ(pool.FindByPath(latest.path)->sample_id, sample);
    Request(REC_DISCARD);
    Block();
    EXPECT_EQ(latest.state, REC_IDLE);
    EXPECT_NE(pool.Find(sample), nullptr);
    EXPECT_FALSE(session.Owns(sample));
}
TEST_F(RecordingSessionTest, StaleTakeAndDuplicateArmCannotReplaceAudio) {
    Request(REC_ARM);
    Block();
    const auto take = latest.take_id;
    RecordOpMessage duplicate;
    duplicate.request_id = take;
    duplicate.op = REC_ARM;
    session.Request(duplicate, false);
    Pump();
    EXPECT_EQ(latest.take_id, take);
    RecordOpMessage stale;
    stale.request_id = id++;
    stale.take_id = take + 42;
    stale.op = REC_DISCARD;
    session.Request(stale, false);
    Pump();
    EXPECT_EQ(latest.error, REC_STALE);
    EXPECT_EQ(latest.take_id, take);
    Request(REC_STOP);
    Block();
    EXPECT_EQ(latest.state, REC_IDLE);
}
TEST_F(RecordingSessionTest, DiscardWaitsForPreviewStopAndPreservesOtherNotes) {
    Capture();
    const int16_t other_pcm[] = {100, 100, 100, 100};
    VoiceTriggerParams other;
    other.sample = other_pcm;
    other.sample_frames = 4;
    other.track = 1;
    const auto other_group = voices.TriggerGroup(&other, 1);
    ASSERT_NE(other_group, 0u);
    Request(REC_AUDITION);
    Block();
    ASSERT_EQ(voices.ActiveVoiceCount(), 2);
    Request(REC_DISCARD);
    EXPECT_EQ(voices.ActiveVoiceCount(), 2);
    Block();
    EXPECT_EQ(voices.ActiveVoiceCount(), 1);
}
TEST_F(RecordingSessionTest, PoolPathCollisionDoesNotCreateAFile) {
    Capture();
    SamplePool::Record* existing = nullptr;
    ASSERT_EQ(pool.AdmitPath("/wavex/recordings/Test take.wav", &existing), SamplePool::Admit::Ok);
    Request(REC_SAVE);
    EXPECT_EQ(latest.error, REC_EXISTS);
    EXPECT_EQ(latest.state, REC_READY);
    EXPECT_FALSE(latest.path[0]);
    FILINFO info{};
    EXPECT_EQ(f_stat("/wavex/recordings/Test take.wav", &info), FR_NO_FILE);
}
TEST_F(RecordingSessionTest, SelectedSourceMetersHoldPeakCountClipsAndResetOnDiscard) {
    Request(REC_ARM, REC_INTERNAL_MIX);
    Block();
    EXPECT_EQ(latest.peak_l, 24576);
    EXPECT_EQ(latest.rms_r, 8192);
    out_l.fill(1.1f);
    Block();
    Request(REC_GET);
    EXPECT_EQ(latest.clip_count, 48u);
    EXPECT_EQ(latest.peak_l, 32767);
    Request(REC_STOP);
    Block();  // No manual start: empty take releases itself.
    EXPECT_EQ(latest.state, REC_IDLE);
    EXPECT_EQ(latest.peak_l, 0);
    EXPECT_EQ(latest.rms_l, 0);
    EXPECT_EQ(latest.clip_count, 0u);
}

}  // namespace
