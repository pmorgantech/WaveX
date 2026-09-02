#pragma once

// Parameter slew engine (roadmap Phase 2.5 item 4; design:
// docs/features/scenes-and-performance.md §3).
//
// A small generic ramp table used anywhere a parameter step would zipper: a
// scene morph, a macro move, a mixer mute, a CC that arrives as one jump. It
// holds no parameter semantics at all - it ramps numbers and hands each one to
// a sink. That is deliberate: §3's requirement is that ramped values reach the
// engine through *the same apply path live CCs use*, so there is no
// scene-only application code to drift out of sync with the live one. An
// engine that applied values itself would be that second path.
//
// Control-tick, not per-sample: a ramp advances once per tick and the value is
// applied at block rate, which is where every destination in
// param-locks-and-modulation.md §3 already lives.
//
// HAL-free, allocation-free, no std::function - the sink is a template
// parameter, same reasoning as instrument.hpp's SampleResolver.

#include <cstdint>

namespace WaveX {
namespace AudioEngine {

/// Concurrent ramps. 32 is scenes-and-performance.md §3's number: a scene
/// morph touches a bounded set, and a table that cannot overflow is worth more
/// than one that can grow.
static constexpr uint8_t kMaxParamRamps = 32;

class ParamSlewEngine {
   public:
    /// Control-tick rate. Ramp lengths are specified in ms, so this is what
    /// converts them into a per-tick step.
    void Init(float tick_hz) { tick_hz_ = tick_hz > 0.0f ? tick_hz : 1000.0f; }

    void Reset() {
        for (uint8_t i = 0; i < kMaxParamRamps; ++i) {
            ramps_[i].active = false;
        }
    }

    /**
     * @brief Starts (or replaces) a ramp for one (param_id, slot).
     *
     * @param from     Starting value, used ONLY when no ramp is already
     *                 running for this target - see the collision rule below.
     * @param morph_ms 0 applies immediately: the sink sees `to` on the next
     *                 Tick() and the ramp does not occupy a slot afterwards.
     * @return false only when the table is full, so a caller can tell the
     *         difference between "ramping" and "silently dropped".
     *
     * **Collision rule (§3): a new ramp for the same target replaces the old
     * one starting from its CURRENT value, not from `from`.** Re-reading the
     * caller's idea of the start point mid-ramp is what produces a jump - the
     * exact artefact this class exists to remove.
     */
    bool Start(uint8_t param_id, uint8_t slot, float from, float to, uint32_t morph_ms) {
        Ramp* existing = Find(param_id, slot);
        const float start = existing ? existing->current : from;

        Ramp* r = existing ? existing : FindFree();
        if (!r) {
            return false;
        }

        r->param_id = param_id;
        r->slot = slot;
        r->current = start;
        r->target = to;
        r->active = true;

        if (morph_ms == 0) {
            r->current = to;
            r->step = 0.0f;
            return true;
        }
        const float ticks = (static_cast<float>(morph_ms) * tick_hz_) / 1000.0f;
        r->step = ticks > 1.0f ? ((to - start) / ticks) : (to - start);
        return true;
    }

    /// Cancels a ramp without applying anything further.
    void Cancel(uint8_t param_id, uint8_t slot) {
        if (Ramp* r = Find(param_id, slot)) {
            r->active = false;
        }
    }

    /**
     * @brief Advances every ramp one tick and applies each through @p apply.
     *
     * @param apply void(uint8_t param_id, uint8_t slot, float value) - the
     *              same function a live CC goes through.
     *
     * A ramp that reaches its target applies once more at exactly the target
     * and then frees its slot, so a morph always ends on the value asked for
     * rather than one step short of it.
     */
    template <typename Sink>
    void Tick(Sink&& apply) {
        for (uint8_t i = 0; i < kMaxParamRamps; ++i) {
            Ramp& r = ramps_[i];
            if (!r.active) {
                continue;
            }
            if (r.step == 0.0f) {
                r.current = r.target;
            } else {
                r.current += r.step;
                // Overshoot check on the side the ramp is travelling, so a
                // step larger than the remaining distance lands on the target
                // instead of oscillating around it.
                if ((r.step > 0.0f && r.current >= r.target) ||
                    (r.step < 0.0f && r.current <= r.target)) {
                    r.current = r.target;
                }
            }
            apply(r.param_id, r.slot, r.current);
            if (r.current == r.target) {
                r.active = false;
            }
        }
    }

    uint8_t ActiveCount() const {
        uint8_t n = 0;
        for (uint8_t i = 0; i < kMaxParamRamps; ++i) {
            if (ramps_[i].active) {
                ++n;
            }
        }
        return n;
    }

    /// Current value of a running ramp, for tests and for the collision rule.
    bool Value(uint8_t param_id, uint8_t slot, float& out) const {
        for (uint8_t i = 0; i < kMaxParamRamps; ++i) {
            const Ramp& r = ramps_[i];
            if (r.active && r.param_id == param_id && r.slot == slot) {
                out = r.current;
                return true;
            }
        }
        return false;
    }

   private:
    struct Ramp {
        uint8_t param_id = 0;
        uint8_t slot = 0;
        float current = 0.0f;
        float target = 0.0f;
        float step = 0.0f;
        bool active = false;
    };

    Ramp* Find(uint8_t param_id, uint8_t slot) {
        for (uint8_t i = 0; i < kMaxParamRamps; ++i) {
            if (ramps_[i].active && ramps_[i].param_id == param_id && ramps_[i].slot == slot) {
                return &ramps_[i];
            }
        }
        return nullptr;
    }

    Ramp* FindFree() {
        for (uint8_t i = 0; i < kMaxParamRamps; ++i) {
            if (!ramps_[i].active) {
                return &ramps_[i];
            }
        }
        return nullptr;
    }

    Ramp ramps_[kMaxParamRamps];
    float tick_hz_ = 1000.0f;
};

}  // namespace AudioEngine
}  // namespace WaveX
