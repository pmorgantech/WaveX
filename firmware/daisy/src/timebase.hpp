#pragma once

struct Timebase {
    static constexpr int kAudioRate = 44100;
    static constexpr int kBlockSize = 48;
    template <typename Fn>
    static inline void Tick1kHz(Fn&& fn) { fn(); }
};


