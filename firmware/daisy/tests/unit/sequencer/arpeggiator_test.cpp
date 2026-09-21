#include "sequencer/arpeggiator.hpp"

#include <gtest/gtest.h>

#include <vector>
using namespace WaveX;
using namespace WaveX::Sequencer;
namespace {
Arpeggiator Chord(uint8_t mode = 0, uint8_t octaves = 1) {
    Arpeggiator arp;
    Arp::Config c;
    c.enabled = 1;
    c.mode = mode;
    c.octaves = octaves;
    arp.Configure(c);
    arp.Press({1, 0, 60}, 70);
    arp.Press({1, 0, 64}, 80);
    arp.Press({1, 0, 67}, 90);
    return arp;
}
std::vector<int> Notes(Arpeggiator& arp, unsigned n) {
    std::vector<int> notes;
    while (n--)
        notes.push_back(arp.Next().note);
    return notes;
}
TEST(Arpeggiator, ExactModeEndpointsAndOctaves) {
    const std::vector<std::vector<int>> expected = {{60, 64, 67, 60, 64, 67, 60, 64},
                                                    {67, 64, 60, 67, 64, 60, 67, 64},
                                                    {60, 64, 67, 67, 64, 60, 60, 64},
                                                    {60, 64, 67, 64, 60, 64, 67, 64},
                                                    {60, 64, 67, 60, 64, 67, 60, 64}};
    for (uint8_t mode = 0; mode < 5; ++mode) {
        auto arp = Chord(mode);
        EXPECT_EQ(Notes(arp, 8), expected[mode]);
    }
    for (uint8_t octaves = 1; octaves <= 4; ++octaves) {
        auto arp = Chord(0, octaves);
        for (uint8_t octave = 0; octave < octaves; ++octave)
            for (auto note: {60, 64, 67})
                EXPECT_EQ(arp.Next().note, note + 12 * octave);
        EXPECT_EQ(arp.Next().note, 60);
    }
}
TEST(Arpeggiator, AsPlayedAndSeededRandomAreDeterministic) {
    auto arp = Chord(4);
    arp.Release({1, 0, 60});
    arp.Press({2, 0, 60}, 100);
    EXPECT_EQ(Notes(arp, 3), (std::vector<int>{64, 67, 60}));
    auto a = Chord(5, 4), b = a;
    a.Restart(123);
    b.Restart(123);
    EXPECT_EQ(Notes(a, 100), Notes(b, 100));
}
TEST(Arpeggiator, LatchReplacementAndRepeatedPitchIdentity) {
    auto arp = Chord();
    auto c = arp.Config();
    c.latch = 1;
    arp.Configure(c);
    arp.Press({1, 0, 60}, 90);
    arp.Press({2, 0, 60}, 100);
    arp.Release({1, 0, 60});
    arp.Press({1, 0, 64}, 110);
    EXPECT_EQ(arp.Count(), 3);
    arp.Release({2, 0, 60});
    arp.Release({1, 0, 64});
    EXPECT_EQ(arp.Count(), 3);
    arp.Press({1, 0, 67}, 120);
    EXPECT_EQ(arp.Count(), 1);
    EXPECT_EQ(arp.Next().note, 67);
    arp.Release({1, 0, 60});
    EXPECT_EQ(arp.Count(), 1);
}
TEST(Arpeggiator, CapacityOverflowPruningAndPitchBounds) {
    auto arp = Chord();
    arp.Clear();
    for (uint8_t n = 0; n < 16; ++n)
        EXPECT_TRUE(arp.Press({1, 0, n}, 100));
    EXPECT_FALSE(arp.Press({1, 0, 16}, 100));
    arp.Prune([](AudioEngine::LiveNoteId id) { return id.note < 8; });
    EXPECT_EQ(arp.Count(), 8);
    arp.Clear();
    auto c = arp.Config();
    c.octaves = 4;
    arp.Configure(c);
    arp.Press({1, 0, 127}, 127);
    for (unsigned i = 0; i < 20; ++i)
        EXPECT_EQ(arp.Next().note, 127);
}
TEST(Arpeggiator, VelocityModesAndDisableClearHeldNotes) {
    auto arp = Chord();
    EXPECT_EQ(arp.Next().velocity, 70);
    auto c = arp.Config();
    c.vel_mode = 1;
    c.vel_fixed = 99;
    arp.Configure(c);
    EXPECT_EQ(arp.Next().velocity, 99);
    c.vel_mode = 2;
    arp.Configure(c);
    arp.Restart();
    EXPECT_EQ(arp.Next().velocity, 127);
    EXPECT_EQ(arp.Next().velocity, 64);
    EXPECT_EQ(arp.Next().velocity, 1);
    c.enabled = 0;
    arp.Configure(c);
    EXPECT_EQ(arp.Count(), 0);
    EXPECT_EQ(arp.Next().velocity, 0);
}
}  // namespace
