#pragma once
#include <cstdint>

namespace WaveX::Allocation {
enum class PlayMode : uint8_t { Poly, Mono };
enum class StealFrom : uint8_t { OwnOnly, OwnFirst, Any };
// Limits count musical groups, including release tails, not oscillator layers.
struct Policy {
    PlayMode mode = PlayMode::Poly;
    uint8_t limit = 0;  // Auto, or 1..8 groups; never expands the global channel budget.
    StealFrom steal = StealFrom::Any;
    bool operator==(const Policy& p) const {
        return mode == p.mode && limit == p.limit && steal == p.steal;
    }
    bool operator!=(const Policy& p) const { return !(*this == p); }
} __attribute__((packed));
static_assert(sizeof(Policy) == 3, "Allocation policy wire fields");
inline bool Valid(const Policy& p) {
    return static_cast<uint8_t>(p.mode) <= 1 && p.limit <= 8 && static_cast<uint8_t>(p.steal) <= 2;
}
struct Override {
    Policy policy;
    bool inherit = true;
    Policy Resolve(const Policy& sound) const { return inherit ? sound : policy; }
    bool operator==(const Override& p) const { return inherit == p.inherit && policy == p.policy; }
    bool operator!=(const Override& p) const { return !(*this == p); }
};
}  // namespace WaveX::Allocation
