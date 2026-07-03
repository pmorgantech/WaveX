#include "cv/cv_group_router.hpp"

#include <gtest/gtest.h>

#include <vector>

namespace {

// Records every QueueGroup()/Flush() call so tests can assert on the fold
// (voice index -> group index) without any real DAC/I2C/SPI hardware.
struct FakeBackend {
    struct Call {
        uint8_t group;
        float cutoff;
        float resonance;
        float vca;
    };
    std::vector<Call> queued;
    int flush_count = 0;

    void QueueGroup(uint8_t group, float cutoff, float resonance, float vca) {
        queued.push_back({group, cutoff, resonance, vca});
    }
    void Flush() { ++flush_count; }
};

}  // namespace

// Stage A: every voice folds onto the single physical group.
TEST(CvGroupRouterTest, SingleGroupFoldsAllVoicesToGroupZero) {
    FakeBackend backend;
    WaveX::Cv::CvGroupRouter<FakeBackend, 1> router(backend);

    router.QueueVoice(0, 0.1f, 0.2f, 0.3f);
    router.QueueVoice(3, 0.4f, 0.5f, 0.6f);
    router.QueueVoice(7, 0.7f, 0.8f, 0.9f);

    ASSERT_EQ(backend.queued.size(), 3u);
    for (const auto& call: backend.queued) {
        EXPECT_EQ(call.group, 0);
    }
}

// Stage B: identity map, voice index == group index.
TEST(CvGroupRouterTest, EightGroupsIsIdentityMap) {
    FakeBackend backend;
    WaveX::Cv::CvGroupRouter<FakeBackend, 8> router(backend);

    for (uint8_t v = 0; v < 8; ++v) {
        router.QueueVoice(v, 0.0f, 0.0f, 0.0f);
    }

    ASSERT_EQ(backend.queued.size(), 8u);
    for (uint8_t v = 0; v < 8; ++v) {
        EXPECT_EQ(backend.queued[v].group, v);
    }
}

// Values are forwarded to the backend unchanged - folding only touches the
// group index, never the CV values themselves.
TEST(CvGroupRouterTest, ForwardsValuesUnchanged) {
    FakeBackend backend;
    WaveX::Cv::CvGroupRouter<FakeBackend, 8> router(backend);

    router.QueueVoice(2, 0.25f, 0.5f, 0.75f);

    ASSERT_EQ(backend.queued.size(), 1u);
    EXPECT_FLOAT_EQ(backend.queued[0].cutoff, 0.25f);
    EXPECT_FLOAT_EQ(backend.queued[0].resonance, 0.5f);
    EXPECT_FLOAT_EQ(backend.queued[0].vca, 0.75f);
}

TEST(CvGroupRouterTest, FlushForwardsToBackend) {
    FakeBackend backend;
    WaveX::Cv::CvGroupRouter<FakeBackend, 1> router(backend);

    router.Flush();
    router.Flush();

    EXPECT_EQ(backend.flush_count, 2);
}

// A voice index at or beyond NumGroups clamps to the last group rather than
// producing an out-of-range group index.
TEST(CvGroupRouterTest, OutOfRangeVoiceClampsToLastGroup) {
    FakeBackend backend;
    WaveX::Cv::CvGroupRouter<FakeBackend, 4> router(backend);

    router.QueueVoice(6, 0.0f, 0.0f, 0.0f);

    ASSERT_EQ(backend.queued.size(), 1u);
    EXPECT_EQ(backend.queued[0].group, 3);
}

TEST(CvGroupRouterTest, FoldVoiceToGroupStaticMapping) {
    using Router1 = WaveX::Cv::CvGroupRouter<FakeBackend, 1>;
    using Router8 = WaveX::Cv::CvGroupRouter<FakeBackend, 8>;

    EXPECT_EQ(Router1::FoldVoiceToGroup(0), 0);
    EXPECT_EQ(Router1::FoldVoiceToGroup(7), 0);

    for (uint8_t v = 0; v < 8; ++v) {
        EXPECT_EQ(Router8::FoldVoiceToGroup(v), v);
    }
}
