#include "midi/event_ring.hpp"

#include <gtest/gtest.h>

#include <thread>
TEST(MidiEventRing, FullNeverOverwritesUnreadAndSlotsAreReused) {
    WaveX::Midi::EventRing<unsigned, 4> ring;
    for (unsigned cycle = 0; cycle < 100; ++cycle) {
        for (unsigned i = 0; i < 4; ++i)
            ASSERT_TRUE(ring.Push(cycle * 4 + i));
        EXPECT_FALSE(ring.Push(999));
        for (unsigned i = 0; i < 4; ++i) {
            unsigned value;
            ASSERT_TRUE(ring.Pop(value));
            EXPECT_EQ(value, cycle * 4 + i);
        }
        unsigned value;
        EXPECT_FALSE(ring.Pop(value));
    }
    EXPECT_EQ(ring.Dropped(), 100u);
}
TEST(MidiEventRing, ProducerAndConsumerPublishCompleteValues) {
    struct Pair {
        unsigned sequence, complement;
    };
    WaveX::Midi::EventRing<Pair, 64> ring;
    std::thread producer([&] {
        for (unsigned i = 0; i < 100000; ++i)
            while (!ring.Push({i, ~i}))
                std::this_thread::yield();
    });
    for (unsigned i = 0; i < 100000; ++i) {
        Pair value;
        while (!ring.Pop(value))
            std::this_thread::yield();
        EXPECT_EQ(value.sequence, i);
        EXPECT_EQ(value.complement, ~i);
    }
    producer.join();
}
