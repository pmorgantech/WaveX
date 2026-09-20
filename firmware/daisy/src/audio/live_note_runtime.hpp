#pragma once
#include "audio/live_note_queue.hpp"
#include "audio/sequencer_voice_map.hpp"

namespace WaveX::AudioEngine {
// One foreground producer (Press/Release), one audio consumer (Drain/StopTracks).
// No pointers are queued. The callback borrows one acquired immutable zone map.
class LiveNoteRuntime {
   public:
    void Init() {
        queue_.Init();
        cutoff_active_ = 0;
        for (auto& cutoff: cutoff_)
            cutoff = 0;
    }
    bool Press(uint8_t source, uint8_t note, uint8_t velocity, uint16_t tracks) {
        return queue_.Press(source, note, velocity, tracks);
    }
    bool Release(uint8_t source, uint8_t note) { return queue_.Release(source, note); }
    uint32_t Refused() const { return queue_.Refused(); }
    bool Drain(const SequencerVoiceMap* map, VoiceManager& voices) {
        bool any_trigger = false;
        // Limit both input events and routed admissions per callback. A 16-Track
        // press is one queue entry and one batch; release events are batch barriers.
        constexpr uint8_t kRoutedBudget = 32, kInputBudget = 32;
        SequencerVoiceMap::Selection selected[kRoutedBudget];
        LiveNoteId identities[kRoutedBudget];
        uint8_t prepared = 0, routed = 0;
        const auto flush = [&] {
            if (!prepared || !map)
                return;
            const auto& prepared_map = *map;
            any_trigger = voices.TriggerBatch(
                              prepared,
                              [&](uint16_t i) { return prepared_map.Describe(selected[i]); },
                              [&](uint16_t i, uint8_t layer, VoiceTriggerParams& params) {
                                  prepared_map.Materialize(selected[i], layer, params);
                                  params.live_note = identities[i];
                              }) != 0 ||
                          any_trigger;
            prepared = 0;
        };
        LiveNoteEvent event;
        for (uint8_t input = 0; input < kInputBudget && queue_.Peek(event); ++input) {
            const auto cost = static_cast<uint8_t>(__builtin_popcount(event.tracks));
            if (event.velocity && routed + cost > kRoutedBudget)
                break;
            queue_.Pop(event);
            if (!event.velocity) {
                flush();
                voices.ReleaseLive(event.id);
                continue;
            }
            routed += cost;
            if (!map)
                continue;
            const auto& prepared_map = *map;
            for (uint8_t track = 0; track < kNumTracks; ++track) {
                if (cutoff_active_ & (1u << track)) {
                    if (static_cast<int32_t>(event.sequence - cutoff_[track]) <= 0)
                        continue;
                    cutoff_active_ &= static_cast<uint16_t>(~(1u << track));
                }
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
        if (!queue_.Peek(event))
            cutoff_active_ = 0;
        return any_trigger;
    }
    void StopTracks(uint16_t tracks, VoiceManager& voices) {
        cutoff_active_ |= tracks;
        for (uint8_t track = 0; track < kNumTracks; ++track)
            if (tracks & (1u << track)) {
                cutoff_[track] = queue_.LastSequence();
                voices.StopTrack(track);
            }
    }

   private:
    LiveNoteQueue<> queue_;
    uint32_t cutoff_[kNumTracks]{};
    uint16_t cutoff_active_ = 0;
};
}  // namespace WaveX::AudioEngine
