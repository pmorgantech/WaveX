#pragma once

// Stage B CV backend: an MCP48CMB28 SPI DAC chain, one physical CV group per
// voice (architecture.md §5.3, docs/features/analog-voice-board.md). Pins
// are already provisioned (WAVEX_DAISY_DAC1_CS..DAC4_CS,
// firmware/shared/config/pin_config.h) but no chain/SPI driver exists yet -
// that bring-up is roadmap Phase 3. This stub only defines the same
// interface as Mcp4728Backend (QueueGroup()/Flush()) so
// WAVEX_CV_BACKEND_MCP48 compiles cleanly in CI ahead of that work.

#include "config/hardware_config.h"

#include "cv_cal.hpp"
#include <array>
#include <cstdint>

namespace WaveX {
namespace Cv {

class Mcp48Backend {
   public:
    void Init() {
        // TODO(roadmap Phase 3): bring up the MCP48CMB28 SPI chain.
    }

    void SetGroupCal(uint8_t group, const CvCal& c) {
        if (group < cal_.size())
            cal_[group] = c;
    }

    // Callback-safe: only stages values.
    void QueueGroup(uint8_t group, float cutoff, float resonance, float vca) {
        if (group >= staged_.size())
            return;
        staged_[group] = {cutoff, resonance, vca};
    }

    // Main-loop only, once implemented - see class comment.
    void Flush() {
        // TODO(roadmap Phase 3): shift staged_[] out over SPI per voice/group.
    }

   private:
    struct StagedGroup {
        float cutoff = 0.0f;
        float resonance = 0.0f;
        float vca = 0.0f;
    };

    std::array<CvCal, WAVEX_ANALOG_CV_GROUPS_MAX> cal_{};
    std::array<StagedGroup, WAVEX_ANALOG_CV_GROUPS_MAX> staged_{};
};

}  // namespace Cv
}  // namespace WaveX
