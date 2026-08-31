#include "audio/snapshot_mailbox.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <thread>

namespace {

struct Snapshot {
    uint32_t generation = 0;
    float a = 0.0f;
    float b = 0.0f;
};

}  // namespace

TEST(SnapshotMailboxTest, InitializedSnapshotIsNotReportedAsAnUpdate) {
    WaveX::AudioEngine::SnapshotMailbox<Snapshot> mailbox;
    mailbox.Init(Snapshot{1, 2.0f, 3.0f});

    Snapshot out;
    EXPECT_FALSE(mailbox.ConsumeLatest(out));
}

TEST(SnapshotMailboxTest, ConsumerReceivesOneCoherentPublishedSnapshot) {
    WaveX::AudioEngine::SnapshotMailbox<Snapshot> mailbox;
    mailbox.Init(Snapshot{});

    mailbox.Publish(Snapshot{7, 11.0f, 13.0f});

    Snapshot out;
    ASSERT_TRUE(mailbox.ConsumeLatest(out));
    EXPECT_EQ(out.generation, 7u);
    EXPECT_FLOAT_EQ(out.a, 11.0f);
    EXPECT_FLOAT_EQ(out.b, 13.0f);
    EXPECT_FALSE(mailbox.ConsumeLatest(out));
}

TEST(SnapshotMailboxTest, TwoPublishesBeforeAConsumeCannotHideTheLatestUpdate) {
    WaveX::AudioEngine::SnapshotMailbox<Snapshot> mailbox;
    mailbox.Init(Snapshot{});

    mailbox.Publish(Snapshot{1, 2.0f, 3.0f});
    mailbox.Publish(Snapshot{2, 5.0f, 8.0f});

    Snapshot out;
    ASSERT_TRUE(mailbox.ConsumeLatest(out));
    EXPECT_EQ(out.generation, 2u);
    EXPECT_FLOAT_EQ(out.a, 5.0f);
    EXPECT_FLOAT_EQ(out.b, 8.0f);
}

TEST(SnapshotMailboxTest, ConcurrentLatestValueCopiesNeverMixSnapshotFields) {
    WaveX::AudioEngine::SnapshotMailbox<Snapshot> mailbox;
    mailbox.Init(Snapshot{});
    std::atomic<bool> producer_done{false};

    std::thread producer([&] {
        for (uint32_t generation = 1; generation <= 200000; ++generation) {
            mailbox.Publish(Snapshot{
                generation, static_cast<float>(generation), static_cast<float>(generation * 2u)});
        }
        producer_done.store(true, std::memory_order_release);
    });

    const auto expect_coherent = [](const Snapshot& snapshot) {
        EXPECT_FLOAT_EQ(snapshot.a, static_cast<float>(snapshot.generation));
        EXPECT_FLOAT_EQ(snapshot.b, static_cast<float>(snapshot.generation * 2u));
    };
    Snapshot out;
    while (!producer_done.load(std::memory_order_acquire)) {
        if (mailbox.ConsumeLatest(out)) {
            expect_coherent(out);
        }
    }
    producer.join();
    while (mailbox.ConsumeLatest(out)) {
        expect_coherent(out);
    }
}
