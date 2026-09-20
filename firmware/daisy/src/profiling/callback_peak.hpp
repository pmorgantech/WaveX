#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace WaveX::Profiling {

// The first nine stages are disjoint. VoiceStart and its four children are
// nested attribution: never add them again when accounting for total time.
enum class CallbackStage : size_t {
    Queue,
    Controls,
    SeqCommands,
    SeqTick,
    SeqResolve,
    SeqLocks,
    SeqTrigger,
    Modulation,
    Render,
    VoiceStart,
    SourceInit,
    FilterInit,
    EnvelopeInit,
    LfoInit,
    Count
};
inline constexpr size_t kCallbackStages = static_cast<size_t>(CallbackStage::Count);
inline constexpr const char* kCallbackStageNames[kCallbackStages] = {"queue",
                                                                     "controls",
                                                                     "seq_commands",
                                                                     "seq_tick",
                                                                     "seq_resolve",
                                                                     "seq_locks",
                                                                     "seq_trigger",
                                                                     "modulation",
                                                                     "render",
                                                                     "voice_start",
                                                                     "source_init",
                                                                     "filter_init",
                                                                     "envelope_init",
                                                                     "lfo_init"};

struct CallbackPeak {
    uint32_t window = 0;
    uint32_t block = 0;
    uint32_t total = 0;
    std::array<uint32_t, kCallbackStages> cycles{};
    std::array<uint32_t, kCallbackStages> calls{};
};

// Callback-owned state. A finished fixed-size window is copied into the
// existing ISR-to-main-loop mailbox; readers never touch this mutable state.
class CallbackPeakWindow {
   public:
    static constexpr uint32_t kWindowBlocks = 5000;

    void Begin() {
        if (in_window_ == 0)
            peak_ = {};
        current_ = {};
        current_.window = window_;
        current_.block = ++blocks_;
    }

    void Add(CallbackStage stage, uint32_t start, uint32_t end) {
        const auto i = static_cast<size_t>(stage);
        current_.cycles[i] += end - start;  // DWT wrap is intentional.
        ++current_.calls[i];
    }

    bool End(uint32_t start, uint32_t end) {
        current_.total = end - start;
        if (current_.total >= peak_.total)
            peak_ = current_;
        if (++in_window_ < kWindowBlocks)
            return false;
        in_window_ = 0;
        ++window_;
        return true;
    }

    const CallbackPeak& Peak() const { return peak_; }

   private:
    CallbackPeak current_{};
    CallbackPeak peak_{};
    uint32_t blocks_ = 0;
    uint32_t window_ = 1;
    uint32_t in_window_ = 0;
};

}  // namespace WaveX::Profiling
