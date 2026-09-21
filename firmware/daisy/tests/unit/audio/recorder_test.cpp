#include "audio/recorder.hpp"

#include <gtest/gtest.h>

#include <array>
#include <thread>
#include <vector>

using WaveX::Recording::Capture;
namespace {
struct RecorderFixture : testing::Test {
    Capture capture;
    std::array<int16_t, 16> history{}, ring{};
    std::array<int16_t, 64> take{};
    void Setup(uint8_t channels = 1,
               uint32_t preroll = 3,
               uint16_t threshold = 0,
               uint32_t maximum = 16,
               uint32_t ring_frames = 8) {
        ASSERT_TRUE(
            capture.Configure({maximum, preroll, threshold, channels},
                              {history.data(), ring.data(), take.data(), ring_frames, 32, 8}));
        capture.Arm();
    }
};
TEST_F(RecorderFixture, ManualStartKeepsLatestPrerollInOrderAndStopDrains) {
    Setup();
    const int16_t before[] = {1, 2, 3, 4, 5};
    capture.Process(before, 5);
    EXPECT_EQ(capture.Drain(100), 0u);
    capture.Start();
    const int16_t after[] = {6, 7};
    capture.Process(after, 2);
    capture.Stop();
    EXPECT_FALSE(capture.Complete());
    EXPECT_EQ(capture.Drain(2), 2u);
    EXPECT_EQ(capture.Drain(100), 3u);
    ASSERT_TRUE(capture.Complete());
    EXPECT_EQ(capture.Reason(), Capture::End::Manual);
    EXPECT_EQ(std::vector<int16_t>(take.begin(), take.begin() + 5),
              (std::vector<int16_t>{3, 4, 5, 6, 7}));
}
TEST_F(RecorderFixture, EitherStereoChannelTriggersAndTriggerFrameOccursOnce) {
    Setup(2, 2, 32768);
    const int16_t pcm[] = {1, 2, 3, 4, 5, -32768, 7, 8};
    capture.Process(pcm, 4);
    capture.Stop();
    capture.Drain(100);
    ASSERT_TRUE(capture.Complete());
    EXPECT_EQ(capture.Frames(), 4u);
    EXPECT_EQ(std::memcmp(pcm, take.data(), sizeof(pcm)), 0);
}
TEST_F(RecorderFixture, MaximumIncludesPrerollAndLeavesGuardSamplesUntouched) {
    take.fill(99);
    Setup(1, 2, 0, 4);
    const int16_t pcm[] = {1, 2, 3, 4, 5};
    capture.Process(pcm, 3);
    capture.Start();
    capture.Process(pcm + 3, 2);
    capture.Drain(100);
    EXPECT_EQ(capture.Reason(), Capture::End::Limit);
    EXPECT_EQ(capture.Frames(), 4u);
    EXPECT_EQ(take[0], 2);
    EXPECT_EQ(take[3], 5);
    EXPECT_EQ(take[4], 99);
}
TEST_F(RecorderFixture, OverflowStopsAndRetainsContiguousPrefix) {
    Setup(1, 0, 0, 16, 2);
    capture.Start();
    const int16_t pcm[] = {1, 2, 3, 4};
    capture.Process(pcm, 4);
    capture.Drain(100);
    ASSERT_TRUE(capture.Complete());
    EXPECT_EQ(capture.Reason(), Capture::End::Overflow);
    EXPECT_EQ(capture.Frames(), 2u);
    EXPECT_EQ(take[0], 1);
    EXPECT_EQ(take[1], 2);
    capture.Process(pcm, 4);
    EXPECT_EQ(capture.Drain(100), 0u);
}
TEST_F(RecorderFixture, DrainWrapsRingWithoutDuplicatingFrames) {
    Setup(1, 0, 0, 12, 3);
    capture.Start();
    for (int16_t i = 0; i < 12; ++i) {
        capture.Process(&i, 1);
        if (i % 2)
            capture.Drain(2);
    }
    capture.Drain(100);
    ASSERT_TRUE(capture.Complete());
    for (int16_t i = 0; i < 12; ++i)
        EXPECT_EQ(take[static_cast<size_t>(i)], i);
}
TEST_F(RecorderFixture, DisarmDoesNotPublishPrerollAsATake) {
    Setup();
    const int16_t pcm[] = {1, 2};
    capture.Process(pcm, 2);
    capture.Stop();
    EXPECT_EQ(capture.Drain(100), 0u);
    EXPECT_TRUE(capture.Complete());
    EXPECT_EQ(capture.Frames(), 0u);
}
TEST_F(RecorderFixture, RejectsInconsistentBuffersAndConfig) {
    Capture::Buffers b{history.data(), ring.data(), take.data(), 8, 32, 8};
    EXPECT_FALSE(capture.Configure({0, 0, 0, 1}, b));
    EXPECT_FALSE(capture.Configure({33, 0, 0, 1}, b));
    EXPECT_FALSE(capture.Configure({8, 8, 0, 1}, b));
    EXPECT_FALSE(capture.Configure({16, 9, 0, 1}, b));
    EXPECT_FALSE(capture.Configure({16, 0, 32769, 1}, b));
    EXPECT_FALSE(capture.Configure({16, 0, 0, 3}, b));
    b.ring_frames = 0;
    EXPECT_FALSE(capture.Configure({16, 0, 0, 1}, b));
}
TEST(RecorderCapture, ProducerAndConsumerPreservePublicationOrder) {
    Capture capture;
    std::array<int16_t, 2048> ring{}, take{};
    ASSERT_TRUE(
        capture.Configure({2048, 0, 0, 1}, {nullptr, ring.data(), take.data(), 2048, 2048, 0}));
    capture.Arm();
    capture.Start();
    std::thread producer([&] {
        for (int16_t i = 0; i < 2048; ++i)
            capture.Process(&i, 1);
    });
    while (!capture.Complete())
        capture.Drain(7);
    producer.join();
    ASSERT_EQ(capture.Frames(), 2048u);
    for (int16_t i = 0; i < 2048; ++i)
        EXPECT_EQ(take[static_cast<size_t>(i)], i);
}
}  // namespace
