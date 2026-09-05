// The Sample Pool's registry (track-and-patch-model.md §4): ids that name
// their slot, path dedupe, Track ownership, and the "never evict" admission.
#include "audio/sample_registry.hpp"

#include <gtest/gtest.h>

#include <set>
#include <string>
#include <vector>

using WaveX::Audio::HashSamplePath;

namespace {

struct Payload {
    uint32_t bytes = 0;
};

constexpr size_t kCap = 1024;
using Registry = WaveX::Audio::SampleRegistry<Payload, kCap>;

struct Fixture : ::testing::Test {
    std::vector<Registry::Record> storage{kCap};
    Registry reg{storage.data()};

    uint16_t admit(const char* path) {
        Registry::Record* r = nullptr;
        EXPECT_EQ(reg.AdmitPath(HashSamplePath(path), &r), Registry::Admit::Ok) << path;
        return r ? r->sample_id : 0;
    }
};

}  // namespace

TEST(SampleRegistryHash, PathsHashAndZeroIsNeverProduced) {
    EXPECT_NE(HashSamplePath("/a.wav"), HashSamplePath("/b.wav"));
    EXPECT_EQ(HashSamplePath("/a.wav"), HashSamplePath("/a.wav"));
    EXPECT_NE(HashSamplePath(""), 0u);
    EXPECT_NE(HashSamplePath(nullptr), 0u);
    EXPECT_NE(HashSamplePath("/A.WAV"), HashSamplePath("/a.wav")) << "case-sensitive on purpose";
}

TEST_F(Fixture, IdsEncodeTheirSlotAndAreNeverZero) {
    const uint16_t a = admit("/one.wav");
    const uint16_t b = admit("/two.wav");
    ASSERT_NE(a, 0);
    ASSERT_NE(b, 0);
    EXPECT_NE(a, b);
    EXPECT_EQ(Registry::SlotOf(a), 0);
    EXPECT_EQ(Registry::SlotOf(b), 1);
    EXPECT_EQ(a >> Registry::kSlotBits, 1) << "first generation is 1";
    EXPECT_EQ(reg.Count(), 2u);
    ASSERT_NE(reg.Find(a), nullptr);
    EXPECT_EQ(reg.Find(a)->sample_id, a);
    EXPECT_EQ(reg.Find(0), nullptr);
}

TEST_F(Fixture, SameFileIsAlreadyResident) {
    const uint16_t a = admit("/kick.wav");
    Registry::Record* r = nullptr;
    EXPECT_EQ(reg.AdmitPath(HashSamplePath("/kick.wav"), &r), Registry::Admit::AlreadyResident);
    ASSERT_NE(r, nullptr);
    EXPECT_EQ(r->sample_id, a);
    EXPECT_EQ(reg.Count(), 1u);
    EXPECT_EQ(reg.FindByPath(HashSamplePath("/kick.wav")), r);
    EXPECT_EQ(reg.FindByPath(HashSamplePath("/snare.wav")), nullptr);
}

TEST_F(Fixture, StaleIdFailsAfterTheSlotIsReused) {
    // Fill every slot so the freed one is the only place the next admission
    // can land - the worst case for aliasing.
    std::vector<uint16_t> ids;
    for (size_t i = 0; i < kCap; ++i) {
        ids.push_back(admit(("/s" + std::to_string(i) + ".wav").c_str()));
    }
    const uint16_t victim = ids[5];
    ASSERT_TRUE(reg.Remove(victim));
    EXPECT_EQ(reg.Find(victim), nullptr);
    EXPECT_FALSE(reg.Remove(victim)) << "removing twice is a stale id";

    const uint16_t reused = admit("/new.wav");
    EXPECT_EQ(Registry::SlotOf(reused), Registry::SlotOf(victim)) << "same slot";
    EXPECT_NE(reused, victim) << "different generation";
    EXPECT_EQ(reg.Find(victim), nullptr) << "the old id must not resolve to the new sample";
    ASSERT_NE(reg.Find(reused), nullptr);
    EXPECT_EQ(reg.Find(reused)->path_hash, HashSamplePath("/new.wav"));
}

TEST_F(Fixture, FreedSlotsAreNotReusedImmediately) {
    const uint16_t a = admit("/a.wav");
    admit("/b.wav");
    reg.Remove(a);
    const uint16_t c = admit("/c.wav");
    EXPECT_NE(Registry::SlotOf(c), Registry::SlotOf(a))
        << "the next slot after the last admission is preferred, so a stale id "
           "has the whole table to survive before it could alias";
}

TEST_F(Fixture, FullIsReportedNotEvicted) {
    for (size_t i = 0; i < kCap; ++i) {
        admit(("/s" + std::to_string(i) + ".wav").c_str());
    }
    Registry::Record* r = nullptr;
    EXPECT_EQ(reg.AdmitPath(HashSamplePath("/one-more.wav"), &r), Registry::Admit::Full);
    EXPECT_EQ(r, nullptr);
    EXPECT_EQ(reg.Count(), kCap);
    // Every earlier entry is untouched.
    for (size_t i = 0; i < kCap; ++i) {
        EXPECT_NE(reg.FindByPath(HashSamplePath(("/s" + std::to_string(i) + ".wav").c_str())),
                  nullptr);
    }
}

TEST_F(Fixture, GenerationWrapsWithoutProducingZeroOrColliding) {
    // Reuse slot 0 more times than the generation field can count.
    std::set<uint16_t> seen;
    uint16_t prev = 0;
    for (int i = 0; i < 70; ++i) {
        Registry::Record* r = nullptr;
        ASSERT_EQ(reg.AdmitPath(HashSamplePath("/x.wav"), &r), Registry::Admit::Ok);
        const uint16_t id = r->sample_id;
        EXPECT_NE(id, 0);
        EXPECT_EQ(Registry::SlotOf(id), 0) << "only slot 0 is ever free here";
        EXPECT_NE(id, prev);
        seen.insert(id);
        prev = id;
        ASSERT_TRUE(reg.Remove(id));
        // Force the next admission back onto slot 0 by leaving it the only
        // free slot: nothing else is admitted, so next_slot_ moved to 1 and
        // wraps around to 0 after scanning 1023 free slots... unless they are
        // free. Fill them once so slot 0 is the sole candidate.
        if (i == 0) {
            for (size_t k = 1; k < kCap; ++k) {
                admit(("/fill" + std::to_string(k) + ".wav").c_str());
            }
        }
    }
    EXPECT_EQ(seen.size(), 63u) << "63 distinct generations, then a wrap";
}

TEST_F(Fixture, TrackOwnershipAndReleasability) {
    const uint16_t a = admit("/piano-c4.wav");
    EXPECT_TRUE(reg.Releasable(a)) << "nobody holds it yet";

    // The user loaded it explicitly: pinned until unloaded.
    EXPECT_TRUE(reg.SetPinned(a, true));
    EXPECT_FALSE(reg.Releasable(a));
    EXPECT_TRUE(reg.SetPinned(a, false));
    EXPECT_TRUE(reg.Releasable(a));

    // Two Tracks reference it (the same sample in two Instruments).
    EXPECT_TRUE(reg.SetUsedBy(a, 3, true));
    EXPECT_TRUE(reg.SetUsedBy(a, 7, true));
    EXPECT_EQ(reg.Find(a)->used_by, (1u << 3) | (1u << 7));
    EXPECT_FALSE(reg.Releasable(a));
    EXPECT_TRUE(reg.SetUsedBy(a, 3, false));
    EXPECT_FALSE(reg.Releasable(a)) << "Track 7 still holds it";
    EXPECT_TRUE(reg.SetUsedBy(a, 7, false));
    EXPECT_TRUE(reg.Releasable(a));

    EXPECT_FALSE(reg.SetUsedBy(a, 16, true)) << "16 Tracks only";
    EXPECT_FALSE(reg.SetUsedBy(0x1234, 0, true)) << "stale id";
}

TEST_F(Fixture, ClearTrackReportsWhatBecameReleasable) {
    const uint16_t shared = admit("/shared.wav");
    const uint16_t only3 = admit("/only3.wav");
    const uint16_t pinned3 = admit("/pinned3.wav");
    reg.SetUsedBy(shared, 3, true);
    reg.SetUsedBy(shared, 5, true);
    reg.SetUsedBy(only3, 3, true);
    reg.SetUsedBy(pinned3, 3, true);
    reg.SetPinned(pinned3, true);

    std::vector<uint16_t> freed;
    reg.ClearTrack(3, [&](uint16_t id) { freed.push_back(id); });
    EXPECT_EQ(freed, std::vector<uint16_t>{only3})
        << "shared is still Track 5's; pinned3 is the user's";
    EXPECT_EQ(reg.Find(shared)->used_by, 1u << 5);
    EXPECT_EQ(reg.Find(only3)->used_by, 0);
    EXPECT_EQ(reg.Find(pinned3)->used_by, 0);
    EXPECT_FALSE(reg.Releasable(pinned3));
}

TEST_F(Fixture, PagesWalkResidentEntriesInSlotOrder) {
    std::vector<uint16_t> ids;
    for (int i = 0; i < 50; ++i) {
        ids.push_back(admit(("/p" + std::to_string(i) + ".wav").c_str()));
    }
    reg.Remove(ids[10]);
    reg.Remove(ids[11]);

    Registry::Record* page[20];
    size_t total = 0;
    size_t n = reg.Page(0, 20, page, &total);
    EXPECT_EQ(total, 48u);
    ASSERT_EQ(n, 20u);
    EXPECT_EQ(page[0]->sample_id, ids[0]);
    EXPECT_EQ(page[10]->sample_id, ids[12]) << "removed entries are skipped, not blanks";

    n = reg.Page(40, 20, page, &total);
    EXPECT_EQ(n, 8u) << "the last page is short";
    EXPECT_EQ(page[7]->sample_id, ids[49]);

    n = reg.Page(48, 20, page, &total);
    EXPECT_EQ(n, 0u);

    size_t visited = 0;
    reg.ForEach([&](Registry::Record& r) {
        ++visited;
        EXPECT_NE(r.sample_id, 0);
    });
    EXPECT_EQ(visited, 48u);
}

TEST_F(Fixture, NewestFollowsRemoval) {
    const uint16_t a = admit("/a.wav");
    reg.NoteNewest(a);
    EXPECT_EQ(reg.Newest(), a);
    reg.Remove(a);
    EXPECT_EQ(reg.Newest(), 0) << "a removed newest is not reported";
}

TEST_F(Fixture, PayloadIsCallerOwnedAndClearedOnAdmit) {
    const uint16_t a = admit("/a.wav");
    reg.Find(a)->payload.bytes = 1234;
    reg.Remove(a);
    // Fill the table so the next admission lands back on slot 0.
    for (size_t i = 1; i < kCap; ++i) {
        admit(("/f" + std::to_string(i) + ".wav").c_str());
    }
    const uint16_t b = admit("/b.wav");
    ASSERT_EQ(Registry::SlotOf(b), 0);
    EXPECT_EQ(reg.Find(b)->payload.bytes, 0u) << "no leftovers from the previous occupant";
}
