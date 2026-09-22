#include "comm/value_queue.h"

#include <gtest/gtest.h>

#include <thread>

TEST(ValueQueue, PreservesWholeValuesAcrossConcurrentPublication) {
    struct Value {
        unsigned sequence;
        unsigned words[64];
    };
    WaveX::Comm::ValueQueue<Value, 4> queue;
    std::thread producer([&] {
        for (unsigned i = 1; i <= 2000; ++i) {
            Value value{};
            value.sequence = i;
            for (auto& word: value.words)
                word = i;
            while (!queue.Push(value))
                std::this_thread::yield();
        }
    });
    for (unsigned i = 1; i <= 2000; ++i) {
        Value value{};
        while (!queue.Pop(value))
            std::this_thread::yield();
        EXPECT_EQ(value.sequence, i);
        for (auto word: value.words)
            EXPECT_EQ(word, i);
    }
    producer.join();
}
TEST(ValueQueue, OverflowIsExplicitAndClearDropsPendingValues) {
    WaveX::Comm::ValueQueue<int, 1> queue;
    EXPECT_TRUE(queue.Push(1));
    EXPECT_FALSE(queue.Push(2));
    EXPECT_TRUE(queue.TakeOverflow());
    EXPECT_FALSE(queue.TakeOverflow());
    int value = 0;
    ASSERT_TRUE(queue.Pop(value));
    EXPECT_EQ(value, 1);
    EXPECT_TRUE(queue.Push(3));
    queue.Clear();
    EXPECT_FALSE(queue.Pop(value));
}
