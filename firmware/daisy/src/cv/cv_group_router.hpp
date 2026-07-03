#pragma once

// CV group router (architecture.md §5.3, roadmap Phase 1 item 1).
//
// Upper layers (sequencer, param locks, UI) always address CV parameters
// per voice (0..7). This router folds that onto WAVEX_ANALOG_CV_GROUPS
// physical CV groups: Stage A has 1 group (paraphonic - every voice folds
// onto the single shared VCF/VCA; which voice's envelope actually drives it
// is the paraphonic envelope law, roadmap Phase 1 item 5, not this router's
// job), Stage B has 8 (identity map, voice index == group index).
//
// Backend is any type exposing:
//   void QueueGroup(uint8_t group, float cutoff, float resonance, float vca);
//   void Flush();
// (Mcp4728Backend for Stage A, Mcp48Backend for Stage B - see
// mcp4728_backend.hpp / mcp48_backend.hpp). Compile-time (template)
// polymorphism, not virtual dispatch, so there's no vtable indirection in
// the control-tick hot path.
//
// Real-time-safety: QueueGroup()/QueueVoice() just stage values and are
// callback-safe. Flush() performs the actual DAC transaction and must only
// ever be called from the main loop (architecture.md §7.1.4 / the
// analog-voice-board.md §0 timing rule) - never from the audio callback.

#include <cstdint>

namespace WaveX {
namespace Cv {

template <typename Backend, uint8_t NumGroups>
class CvGroupRouter {
   public:
    static_assert(NumGroups >= 1, "CvGroupRouter needs at least one CV group");

    explicit CvGroupRouter(Backend& backend) : backend_(backend) {}

    // Fold a voice-indexed CV target onto its physical group and stage it.
    void QueueVoice(uint8_t voice, float cutoff, float resonance, float vca) {
        backend_.QueueGroup(FoldVoiceToGroup(voice), cutoff, resonance, vca);
    }

    // Main-loop only - see class comment.
    void Flush() { backend_.Flush(); }

    // Exposed for tests and callers that want to reason about the mapping
    // directly. NumGroups == 1 folds every voice onto group 0 (Stage A).
    // NumGroups >= voice index is the Stage B identity map; voices beyond
    // NumGroups clamp to the last group rather than going out of bounds.
    static constexpr uint8_t FoldVoiceToGroup(uint8_t voice) {
        return NumGroups == 1 ? uint8_t{0} : (voice < NumGroups ? voice : uint8_t(NumGroups - 1));
    }

   private:
    Backend& backend_;
};

}  // namespace Cv
}  // namespace WaveX
