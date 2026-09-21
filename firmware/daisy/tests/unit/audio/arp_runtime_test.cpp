#include "audio/arp_runtime.hpp"

#include <gtest/gtest.h>

#include "sequencer/sequencer_scheduler.hpp"
#include <memory>
using namespace WaveX;
using namespace WaveX::AudioEngine;
namespace {
struct ArpRuntimeTest : testing::Test {
    std::unique_ptr<SequencerVoiceMap> map = std::make_unique<SequencerVoiceMap>();
    ArpRuntime runtime;
    VoiceManager voices;
    Sequencer::SequencerScheduler clock;
    Sequencer::Pattern pattern;
    static void Record(uint8_t, uint8_t, uint32_t, uint8_t, uint16_t) {}
    void SetUp() override {
        voices.Init(48000);
        clock.Init(48000, 48);
        clock.SetTempo(120);
        clock.SetPattern(&pattern);
        map->tracks[0].arp.enabled = 1;
        runtime.Sync(map.get(), voices, Record);
        LiveNoteEvent event{{1, 0, 60}, 1, 3, 100};
        runtime.Input(event);
        EXPECT_EQ(event.tracks, 2);  // The other Track still receives the physical key.
    }
};
TEST_F(ArpRuntimeTest, FreeRunAndTransportUseExactSampleGridForTenMinutes) {
    for (bool playing: {false, true}) {
        ArpRuntime arp;
        arp.Sync(map.get(), voices, Record);
        LiveNoteEvent press{{1, 0, 60}, 1, 1, 100};
        arp.Input(press);
        if (playing)
            clock.Start();
        uint64_t notes = 0;
        for (uint64_t block = 0; block < 600000; ++block) {
            Sequencer::TriggerEvent output[16], unused[64];
            const auto start = clock.CurrentFrame();
            const auto tick = clock.PositionTicks();
            const auto n = arp.Events(
                clock, start, tick, 48, playing, clock.RunEpoch(), output, voices, Record);
            if (n) {
                EXPECT_EQ(n, 1u);
                const uint64_t frame = block * 48 + output[0].frame - start;
                EXPECT_EQ(frame, notes * 6000);
                ++notes;
            }
            if (playing)
                clock.Process(unused, 64);
        }
        EXPECT_EQ(notes, 4800u);
    }
}
TEST_F(ArpRuntimeTest, ReleaseDisableAndRebindRetireOnlyArpOwnedGroups) {
    int16_t pcm[1024]{};
    VoiceTriggerParams p;
    p.sample = pcm;
    p.sample_frames = 1024;
    p.one_shot = true;
    const auto other = voices.TriggerGroup(&p, 1);
    const auto own = voices.TriggerGroup(&p, 1);
    ASSERT_NE(other, 0u);
    ASSERT_NE(own, 0u);
    Sequencer::TriggerEvent e;
    e.track = 0;
    e.note = 60;
    e.velocity = 100;
    e.gate_ticks = 18;
    unsigned releases = 0;
    auto record = [&](uint8_t source, uint8_t, uint32_t, uint8_t velocity, uint16_t) {
        EXPECT_EQ(source, 32);
        if (!velocity)
            ++releases;
    };
    runtime.Admit(e, own, voices, record);
    runtime.Stop(1, voices, record);
    EXPECT_EQ(releases, 1u);
    EXPECT_EQ(voices.GetVoice(0).sequence_release_offset, UINT16_MAX);
    EXPECT_EQ(voices.GetVoice(1).sequence_release_offset, 0);
}
}  // namespace
