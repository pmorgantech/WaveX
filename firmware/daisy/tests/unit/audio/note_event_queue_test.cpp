#include "audio/note_event_queue.hpp"

#include <gtest/gtest.h>

namespace {

struct TestNoteEvent {
    bool is_trigger = false;
    uint8_t note = 0;
};

using TestQueue = WaveX::AudioEngine::NoteEventQueue<TestNoteEvent, 4>;

}  // namespace

TEST(NoteEventQueueTest, FullQueueRemembersNoteOffForCallbackRecovery) {
    TestQueue queue;
    queue.Init();

    for (uint8_t note = 0; note < 4; ++note) {
        EXPECT_TRUE(queue.Push(TestNoteEvent{true, note}));
    }

    EXPECT_FALSE(queue.PushReleaseOrRemember(TestNoteEvent{false, 60}));

    TestNoteEvent event;
    for (uint8_t note = 0; note < 4; ++note) {
        ASSERT_TRUE(queue.Pop(event));
        EXPECT_TRUE(event.is_trigger);
        EXPECT_EQ(event.note, note);
    }
    EXPECT_FALSE(queue.Pop(event));
    EXPECT_EQ(queue.TakeOverflowReleaseWord(1), 1u << (60u - 32u));
    EXPECT_EQ(queue.TakeOverflowReleaseWord(1), 0u);
}

TEST(NoteEventQueueTest, RepeatedOverflowNoteOffsCoalesceWithoutLosingTheNote) {
    TestQueue queue;
    queue.Init();
    for (uint8_t note = 0; note < 4; ++note) {
        ASSERT_TRUE(queue.Push(TestNoteEvent{true, note}));
    }

    EXPECT_FALSE(queue.PushReleaseOrRemember(TestNoteEvent{false, 127}));
    EXPECT_FALSE(queue.PushReleaseOrRemember(TestNoteEvent{false, 127}));

    EXPECT_EQ(queue.TakeOverflowReleaseWord(3), 1u << 31u);
}

TEST(NoteEventQueueTest, FullQueueDoesNotTurnDroppedTriggersIntoReleases) {
    TestQueue queue;
    queue.Init();
    for (uint8_t note = 0; note < 4; ++note) {
        ASSERT_TRUE(queue.Push(TestNoteEvent{true, note}));
    }

    EXPECT_FALSE(queue.Push(TestNoteEvent{true, 64}));
    for (uint32_t word = 0; word < TestQueue::kReleaseWordCount; ++word) {
        EXPECT_EQ(queue.TakeOverflowReleaseWord(word), 0u);
    }
}
