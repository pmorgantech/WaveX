#pragma once
#include "instrument.hpp"
#include <algorithm>
namespace WaveX::AudioEngine {
// Foreground-owned undo of sound settings. Sample/key maps have their own
// assignment lifecycle: this snapshot holds no PCM pointers or Pool references.
class InstrumentSoundUndo {
   public:
    bool Active() const { return active_; }
    void Apply() { active_ = false; }
    void Capture(const Instrument& ins) {
        if (active_)
            return;
        allocation_ = ins.allocation;
        arp_ = ins.arp;
        filter_ = ins.filter;
        std::copy_n(ins.env, 3, env_);
        std::copy_n(ins.lfo, 2, lfo_);
        std::copy_n(ins.mod_slots, kMaxModSlots, slots_);
        gain_ = ins.trim_gain;
        pan_ = ins.trim_pan;
        mix_ = ins.osc_mix;
        transpose_ = ins.transpose;
        fine_ = ins.fine_tune;
        for (uint8_t i = 0; i < 2; ++i) {
            const auto& o = ins.osc[i];
            osc_[i] = {o.level, o.pan, o.coarse_tune, o.fine_tune, o.keytrack, o.mono};
        }
        active_ = true;
    }
    bool Revert(Instrument& ins) {
        if (!active_)
            return false;
        ins.allocation = allocation_;
        ins.arp = arp_;
        ins.filter = filter_;
        std::copy_n(env_, 3, ins.env);
        std::copy_n(lfo_, 2, ins.lfo);
        std::copy_n(slots_, kMaxModSlots, ins.mod_slots);
        ins.trim_gain = gain_;
        ins.trim_pan = pan_;
        ins.osc_mix = mix_;
        ins.transpose = transpose_;
        ins.fine_tune = fine_;
        for (uint8_t i = 0; i < 2; ++i) {
            auto& o = ins.osc[i];
            const auto& s = osc_[i];
            o.level = s.level;
            o.pan = s.pan;
            o.coarse_tune = s.coarse;
            o.fine_tune = s.fine;
            o.keytrack = s.keytrack;
            o.mono = s.mono;
        }
        active_ = false;
        return true;
    }

   private:
    struct OscSettings {
        float level = 1, pan = .5f;
        int8_t coarse = 0, fine = 0;
        uint8_t keytrack = 1;
        bool mono = false;
    };
    Allocation::Policy allocation_;
    Arp::Config arp_;
    InstrumentFilter filter_;
    InstrumentEnv env_[3];
    InstrumentLfo lfo_[2];
    ModSlot slots_[kMaxModSlots];
    OscSettings osc_[2];
    float gain_ = 1, pan_ = .5f, mix_ = 0;
    int8_t transpose_ = 0, fine_ = 0;
    bool active_ = false;
};
}  // namespace WaveX::AudioEngine
