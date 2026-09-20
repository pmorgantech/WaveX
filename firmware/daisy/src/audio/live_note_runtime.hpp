#pragma once
#include "audio/live_note_queue.hpp"
#include "audio/mono_held_keys.hpp"
#include "audio/sequencer_voice_map.hpp"

namespace WaveX::AudioEngine {
// One foreground producer (Press/Release), one audio consumer (Drain/StopTracks).
// No pointers are queued. The callback borrows one acquired immutable zone map.
class LiveNoteRuntime {
   public:
    void Init() {
        queue_.Init();
        held_.Init();
        mono_ = 0;
        cutoff_active_ = 0;
        for (auto& cutoff: cutoff_)
            cutoff = 0;
    }
    bool Press(uint8_t source, uint8_t note, uint8_t velocity, uint16_t tracks) {
        return queue_.Press(source, note, velocity, tracks);
    }
    bool Release(uint8_t source, uint8_t note) { return queue_.Release(source, note); }
    uint32_t Refused() const { return queue_.Refused(); }
    struct IgnoreInput {
        void operator()(const LiveNoteEvent&) const {}
    };
    bool OverflowReleased(LiveNoteId id) const { return queue_.OverflowReleased(id); }
    template <typename Observe = IgnoreInput>
    bool Drain(const SequencerVoiceMap* map, VoiceManager& voices, Observe observe = {}) {
        bool any_trigger = false;
        uint16_t mono = 0;
        if (map)
            for (uint8_t t = 0; t < kNumTracks; ++t)
                if (map->tracks[t].policy.mode == Allocation::PlayMode::Mono)
                    mono |= static_cast<uint16_t>(1u << t);
        if (mono != mono_)
            held_.Clear(static_cast<uint16_t>(mono ^ mono_));
        mono_ = mono;
        // Limit both input events and routed admissions per callback. A 16-Track
        // press is one queue entry and one batch; release events are batch barriers.
        constexpr uint8_t kRoutedBudget = 32, kInputBudget = 32;
        const uint8_t input_budget = mono_ ? 16 : kRoutedBudget;
        SequencerVoiceMap::Selection selected[kRoutedBudget];
        LiveNoteId identities[kRoutedBudget];
        uint8_t prepared = 0, routed = 0;
        const auto gated = [&](const SequencerVoiceMap::Selection& s) {
            if (!(mono_ & (1u << s.track)) || !map->tracks[s.track].keyboard)
                return false;
            for (uint8_t layer = 0; layer < s.count; ++layer) {
                const bool primary = s.zones[0][layer] != SequencerVoiceMap::kNoZone;
                const auto& osc = primary
                                      ? static_cast<const SequencerVoiceMap::PreparedOscillator&>(
                                            map->tracks[s.track])
                                      : map->tracks[s.track].secondary;
                if (!osc.zones[s.zones[primary ? 0 : 1][layer]].one_shot)
                    return true;
            }
            return false;
        };
        const auto flush = [&] {
            if (!prepared || !map)
                return;
            const auto& prepared_map = *map;
            any_trigger = voices.TriggerBatch(
                              prepared,
                              [&](uint16_t i) {
                                  auto description = prepared_map.Describe(selected[i]);
                                  if (gated(selected[i]) && !held_.Available())
                                      description.count = 0;
                                  return description;
                              },
                              [&](uint16_t i, uint8_t layer, VoiceTriggerParams& params) {
                                  prepared_map.Materialize(selected[i], layer, params);
                                  params.live_note = identities[i];
                              },
                              [&](uint16_t i, uint64_t) {
                                  const auto& s = selected[i];
                                  if (gated(s))
                                      held_.Add(s.track, identities[i], s.velocity);
                                  else if (mono_ & (1u << s.track))
                                      held_.Clear(static_cast<uint16_t>(1u << s.track));
                              }) != 0 ||
                          any_trigger;
            prepared = 0;
        };
        LiveNoteEvent event;
        for (uint8_t input = 0; input < kInputBudget && queue_.Peek(event); ++input) {
            const auto cost = static_cast<uint8_t>(__builtin_popcount(event.tracks));
            if (event.velocity && routed + cost > input_budget)
                break;
            queue_.Pop(event);
            // Recording and playback share the same surviving destinations.
            // A binding change must not capture queued presses for its old Track.
            if (event.velocity)
                for (uint8_t track = 0; track < kNumTracks; ++track)
                    if (cutoff_active_ & (1u << track)) {
                        if (static_cast<int32_t>(event.sequence - cutoff_[track]) <= 0)
                            event.tracks &= static_cast<uint16_t>(~(1u << track));
                        else
                            cutoff_active_ &= static_cast<uint16_t>(~(1u << track));
                    }
            observe(event);
            if (!event.velocity) {
                flush();
                voices.ReleaseLive(event.id);
                if (mono_)
                    held_.Remove([&](LiveNoteId id) { return id.Matches(event.id); });
                continue;
            }
            routed += cost;
            if (!map)
                continue;
            const auto& prepared_map = *map;
            for (uint8_t track = 0; track < kNumTracks; ++track) {
                if (!(event.tracks & (1u << track)))
                    continue;
                selected[prepared] = prepared_map.Select(track, event.id.note, event.velocity);
                identities[prepared++] = event.id;
            }
        }
        flush();
        // An overflow watermark also covers older events still queued behind this
        // callback's work budget. Checking every block prevents their late arrival
        // from resurrecting a released gated note. One-shots retain their semantics.
        voices.ReleaseOverflow([this](LiveNoteId id) { return queue_.OverflowReleased(id); });
        if (mono_) {
            held_.Remove([this](LiveNoteId id) { return queue_.OverflowReleased(id); });
            // At most one full-envelope fallback per Track after this block's
            // releases. Sixteen input admissions plus sixteen fallback admissions
            // retain the previous overall 32-request callback work ceiling.
            prepared = 0;
            for (uint8_t track = 0; map && track < kNumTracks; ++track)
                if (const auto* key = held_.Fallback(track)) {
                    selected[prepared] = map->Select(track, key->id.note, key->velocity);
                    identities[prepared++] = key->id;
                }
            if (prepared)
                any_trigger = voices.TriggerBatch(
                                  prepared,
                                  [&](uint16_t i) { return map->Describe(selected[i]); },
                                  [&](uint16_t i, uint8_t layer, VoiceTriggerParams& params) {
                                      map->Materialize(selected[i], layer, params);
                                      params.live_note = identities[i];
                                  }) != 0 ||
                              any_trigger;
            held_.FinishFallbacks();
        }
        if (!queue_.Peek(event))
            cutoff_active_ = 0;
        return any_trigger;
    }
    void StopTracks(uint16_t tracks, VoiceManager& voices) {
        cutoff_active_ |= tracks;
        held_.Clear(tracks);
        for (uint8_t track = 0; track < kNumTracks; ++track)
            if (tracks & (1u << track)) {
                cutoff_[track] = queue_.LastSequence();
                voices.StopTrack(track);
            }
    }

   private:
    LiveNoteQueue<> queue_;
    MonoHeldKeys held_;
    uint16_t mono_ = 0;
    uint32_t cutoff_[kNumTracks]{};
    uint16_t cutoff_active_ = 0;
};
}  // namespace WaveX::AudioEngine
