#pragma once

// Audio-rate/control-tick identity (architecture.md §5.1): 48 samples at
// 48 kHz = exactly 1.0 ms per block, so one audio callback == one 1 kHz
// control tick, and CV/modulation/sequencer timing is phase-locked to the
// audio stream. This 1-block = 1-ms identity is a design invariant: any
// change to either constant must preserve an integer-ms tick or introduce
// a real tick divider (counting samples, not blocks). The engine briefly
// ran at 44.1 kHz, which silently made this "1 kHz" tick 918.75 Hz — see
// docs/dma-timing-review-2026-07-03.md Finding 1 for why that was reverted.
struct Timebase {
    static constexpr int kAudioRate = 48000;
    static constexpr int kBlockSize = 48;
    static_assert((kBlockSize * 1000) % kAudioRate == 0,
                  "block must be an integer number of milliseconds - see architecture.md §5.1");
    template <typename Fn>
    static inline void Tick1kHz(Fn&& fn) {
        fn();
    }
};
