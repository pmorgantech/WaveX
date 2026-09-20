#include <gtest/gtest.h>

#include "audio/voice_manager.hpp"

using namespace WaveX::AudioEngine;

namespace {
int16_t pcm[256]{};
VoiceTriggerParams Layer(uint8_t track = 0, bool stereo = false) {
    VoiceTriggerParams p;
    p.sample = pcm;
    p.sample_frames = 128;
    p.channels = stereo ? 2 : 1;
    p.track = track;
    p.note = 60;
    p.loop = true;
    p.attack_s = 0;
    p.release_s = .001f;
    return p;
}
unsigned Members(const VoiceManager& voices, uint64_t id, bool releasing = false) {
    unsigned result = 0;
    for (uint8_t i = 0; i < WAVEX_NUM_VOICES; ++i) {
        const auto& voice = voices.GetVoice(i);
        if (!voice.IsFree() && voice.group_id == id && voice.envelope.IsReleasing() == releasing)
            ++result;
    }
    return result;
}
}  // namespace

TEST(VoiceGroup, StereoLayerStealRetiresTheEntireOldNote) {
    VoiceManager voices;
    voices.Init(48000);
    VoiceTriggerParams layers[4] = {Layer(0, true), Layer(0, true), Layer(0, true), Layer(0, true)};
    const auto old = voices.TriggerGroup(layers, 4);
    ASSERT_NE(old, 0u);
    EXPECT_EQ(voices.ActiveChannelCount(), 8);
    auto mono = Layer(1);
    const auto current = voices.TriggerGroup(&mono, 1);
    ASSERT_NE(current, 0u);
    EXPECT_NE(current, old);
    EXPECT_EQ(voices.ActiveVoiceCount(), 1);
    EXPECT_EQ(voices.ActiveChannelCount(), 1);
    EXPECT_EQ(Members(voices, old), 0u);
    voices.ReleaseGroup(old);
    EXPECT_EQ(Members(voices, current), 1u);
}

TEST(VoiceGroup, RefusalLeavesExistingVoicesAndChokesUntouched) {
    VoiceManager voices;
    voices.Init(48000);
    auto own = Layer(0);
    own.choke_group = 2;
    const auto held = voices.TriggerGroup(&own, 1);
    auto foreign = Layer(1);
    for (int i = 0; i < 7; ++i)
        voices.Trigger(foreign);
    VoiceTriggerParams layers[2] = {Layer(0, true), Layer(0, true)};
    layers[0].choke_group = 2;
    Allocation::Policy policy;
    policy.steal = Allocation::StealFrom::OwnOnly;
    EXPECT_EQ(voices.TriggerGroup(layers, 2, policy), 0u);
    EXPECT_EQ(Members(voices, held), 1u);
    EXPECT_EQ(voices.ActiveChannelCount(), 8);
    layers[1].sample = nullptr;
    EXPECT_EQ(voices.TriggerGroup(layers, 2), 0u);
    EXPECT_EQ(Members(voices, held), 1u);
    EXPECT_EQ(voices.ActiveChannelCount(), 8);
}

TEST(VoiceGroup, ChokeDoesNotReleaseSiblingLayers) {
    VoiceManager voices;
    voices.Init(48000);
    VoiceTriggerParams layers[2] = {Layer(), Layer()};
    layers[0].choke_group = layers[1].choke_group = 3;
    const auto first = voices.TriggerGroup(layers, 2);
    EXPECT_EQ(Members(voices, first), 2u);
    const auto second = voices.TriggerGroup(layers, 2);
    EXPECT_EQ(Members(voices, first, true), 2u);
    EXPECT_EQ(Members(voices, second), 2u);
}

TEST(VoiceGroup, IdentityReleaseKeepsRepeatedNotesAndOneShotsIndependent) {
    VoiceManager voices;
    voices.Init(48000);
    VoiceTriggerParams layers[2] = {Layer(), Layer()};
    const auto first = voices.TriggerGroup(layers, 2);
    layers[1].one_shot = true;
    const auto second = voices.TriggerGroup(layers, 2);
    voices.ReleaseGroup(first);
    EXPECT_EQ(Members(voices, first, true), 2u);
    EXPECT_EQ(Members(voices, second), 2u);
    voices.Release(60);
    EXPECT_EQ(Members(voices, second, true), 1u);
    EXPECT_EQ(Members(voices, second), 1u);
}

TEST(VoiceGroup, PlannedChokeReclaimsItsWholeGroupBeforeUnrelatedHeldNotes) {
    VoiceManager voices;
    voices.Init(48000);
    auto unrelated = Layer(1);
    uint64_t older[6];
    for (auto& id: older)
        id = voices.TriggerGroup(&unrelated, 1);
    VoiceTriggerParams hats[2] = {Layer(), Layer()};
    hats[0].choke_group = hats[1].choke_group = 3;
    const auto open_hat = voices.TriggerGroup(hats, 2);
    ASSERT_EQ(voices.ActiveVoiceCount(), 8);
    const auto closed_hat = voices.TriggerGroup(hats, 1);
    EXPECT_EQ(voices.ActiveVoiceCount(), 7);
    EXPECT_EQ(Members(voices, open_hat), 0u);
    EXPECT_EQ(Members(voices, closed_hat), 1u);
    for (const auto id: older)
        EXPECT_EQ(Members(voices, id), 1u);
}

TEST(VoiceGroup, PartialLayerCompletionKeepsRemainingReservationsGrouped) {
    VoiceManager voices;
    voices.Init(48000);
    VoiceTriggerParams layers[2] = {Layer(), Layer()};
    layers[0].loop = false;
    layers[0].sample_frames = 2;
    layers[0].release_s = 0;
    const auto old = voices.TriggerGroup(layers, 2);
    float left[48], right[48];
    voices.Render(left, right, 48);
    ASSERT_EQ(voices.ActiveVoiceCount(), 1);
    EXPECT_EQ(Members(voices, old), 1u);
    auto other = Layer(1);
    for (int i = 0; i < 7; ++i)
        voices.Trigger(other);
    voices.Trigger(other);
    EXPECT_EQ(Members(voices, old), 0u);
    EXPECT_EQ(voices.ActiveVoiceCount(), 8);
}

TEST(VoiceGroup, LocalCapsCountNotesAndRetirementCannotReleaseReplacement) {
    VoiceManager voices;
    voices.Init(48000);
    VoiceTriggerParams layers[2] = {Layer(), Layer()};
    Allocation::Policy mono;
    mono.mode = Allocation::PlayMode::Mono;
    const auto old = voices.TriggerGroup(layers, 2, mono);
    const auto replacement = voices.TriggerGroup(layers, 2, mono);
    EXPECT_EQ(Members(voices, old), 0u);
    EXPECT_EQ(Members(voices, replacement), 2u);
    voices.StopTrack(0);
    const auto rebound = voices.TriggerGroup(layers, 2, mono);
    voices.ReleaseGroup(replacement);
    EXPECT_EQ(Members(voices, rebound), 2u);
    voices.StopAll();
    const auto after_stop = voices.TriggerGroup(layers, 2, mono);
    EXPECT_NE(after_stop, rebound);
    voices.ReleaseGroup(rebound);
    EXPECT_EQ(Members(voices, after_stop), 2u);
}

TEST(VoiceGroup, RejectsMixedIdentityAndOverBudgetNotes) {
    VoiceManager voices;
    voices.Init(48000);
    VoiceTriggerParams layers[5] = {
        Layer(0, true), Layer(0, true), Layer(0, true), Layer(0, true), Layer(0, true)};
    EXPECT_EQ(voices.TriggerGroup(layers, 5), 0u);
    layers[1].track = 1;
    EXPECT_EQ(voices.TriggerGroup(layers, 2), 0u);
    layers[1].track = 0;
    layers[1].trigger_note = 61;
    EXPECT_EQ(voices.TriggerGroup(layers, 2), 0u);
    layers[1].trigger_note = 60;
    layers[1].start_offset_frames = 7;
    EXPECT_EQ(voices.TriggerGroup(layers, 2), 0u);
    EXPECT_EQ(voices.ActiveVoiceCount(), 0);
}

TEST(VoiceGroup, SameFrameBatchMaterializesOnlySurvivingLayers) {
    VoiceManager voices;
    voices.Init(48000);
    unsigned materialized = 0;
    const auto last = voices.TriggerBatch(
        16,
        [](uint16_t request) {
            TriggerDescription note;
            note.track = static_cast<uint8_t>(request);
            note.count = 4;
            return note;
        },
        [&](uint16_t request, uint8_t, VoiceTriggerParams& p) {
            ++materialized;
            p = Layer(static_cast<uint8_t>(request));
            p.start_offset_frames = 17;
        });
    EXPECT_EQ(last, 16u);
    EXPECT_EQ(materialized, 8u);
    EXPECT_EQ(voices.ActiveChannelCount(), 8u);
    for (uint8_t i = 0; i < WAVEX_NUM_VOICES; ++i) {
        const auto& v = voices.GetVoice(i);
        EXPECT_GE(v.track, 14);
        EXPECT_EQ(v.start_offset_frames, 17);
        EXPECT_EQ(v.age, 56u + i);
    }
}

TEST(VoiceGroup, BatchMatchesSequentialAdmissionChokesRandomnessAndAudio) {
    // Deterministic differential workload: whole groups, partial chokes,
    // stereo, caps, refusals, release tails and reused slots across batches.
    VoiceManager sequential, batched;
    sequential.Init(48000);
    batched.Init(48000);
    uint32_t random = 71237;
    const auto next = [&]() {
        random ^= random << 13;
        random ^= random >> 17;
        random ^= random << 5;
        return random;
    };
    for (auto& sample: pcm)
        sample = static_cast<int16_t>(next() & 0x7fff);
    for (unsigned round = 0; round < 100; ++round) {
        SCOPED_TRACE(round);
        VoiceTriggerParams notes[16][4];
        TriggerDescription descriptions[16];
        uint64_t expected_id = 0;
        for (uint8_t n = 0; n < 16; ++n) {
            auto& description = descriptions[n];
            description.track = static_cast<uint8_t>(next() % 4);
            description.count = static_cast<uint8_t>(1 + next() % 4);
            description.policy.limit = static_cast<uint8_t>(next() % 9);
            description.policy.mode = static_cast<Allocation::PlayMode>(next() % 2);
            description.policy.steal = static_cast<Allocation::StealFrom>(next() % 3);
            for (uint8_t i = 0; i < description.count; ++i) {
                auto& p = notes[n][i];
                p = Layer(description.track, next() % 2);
                p.choke_group = static_cast<uint8_t>(next() % 4);
                p.one_shot = next() % 2;
                p.start_offset_frames = 13;
                p.lfo[0].wave = 4;  // sample and hold
                description.layers[i] = {p.channels, p.choke_group};
            }
            const auto id =
                sequential.TriggerGroup(notes[n], description.count, description.policy);
            if (id)
                expected_id = id;
        }
        const auto actual_id = batched.TriggerBatch(
            16,
            [&](uint16_t n) { return descriptions[n]; },
            [&](uint16_t n, uint8_t layer, VoiceTriggerParams& p) { p = notes[n][layer]; });
        ASSERT_EQ(actual_id, expected_id);
        for (uint8_t i = 0; i < WAVEX_NUM_VOICES; ++i) {
            const auto& a = sequential.GetVoice(i);
            const auto& b = batched.GetVoice(i);
            ASSERT_EQ(a.IsFree(), b.IsFree());
            if (a.IsFree())
                continue;
            EXPECT_EQ(a.group_id, b.group_id);
            EXPECT_EQ(a.age, b.age);
            EXPECT_EQ(a.track, b.track);
            EXPECT_EQ(a.render_channels, b.render_channels);
            EXPECT_EQ(a.choke_group, b.choke_group);
            EXPECT_EQ(a.envelope.IsReleasing(), b.envelope.IsReleasing());
            EXPECT_FLOAT_EQ(a.mod_random, b.mod_random);
        }
        float expected_l[48], expected_r[48], actual_l[48], actual_r[48];
        sequential.TickModulation({}, {}, 48);
        batched.TickModulation({}, {}, 48);
        sequential.Render(expected_l, expected_r, 48);
        batched.Render(actual_l, actual_r, 48);
        for (unsigned i = 0; i < 48; ++i) {
            EXPECT_FLOAT_EQ(expected_l[i], actual_l[i]);
            EXPECT_FLOAT_EQ(expected_r[i], actual_r[i]);
        }
        if (round % 3 == 0) {
            sequential.ReleaseGroup(expected_id);
            batched.ReleaseGroup(actual_id);
        }
    }
    for (auto& sample: pcm)
        sample = 0;
}
