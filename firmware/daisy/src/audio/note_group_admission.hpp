#pragma once

#include "config/hardware_config.h"

#include <array>
#include <cstddef>
#include <cstdint>

namespace WaveX::AudioEngine::Allocation {
enum class PlayMode : uint8_t { Poly, Mono };
enum class StealFrom : uint8_t { OwnOnly, OwnFirst, Any };

// Resolved sound/Track policy. Persistence and inheritance are separate from
// admission; no legacy file fields are reinterpreted by this model.
struct Policy {
    PlayMode mode = PlayMode::Poly;
    uint8_t limit = 0;  // Auto, or 1..8 musical groups (release tails included)
    StealFrom steal = StealFrom::Any;
};
struct Owner {
    uint32_t binding = 0;  // nonzero Track binding generation, never a Sample ID
    uint8_t track = 0;
    bool operator==(const Owner& other) const {
        return track == other.track && binding == other.binding;
    }
};
struct Group {
    // Unique monotonically increasing admission identity also orders onsets.
    // Zero is reserved. The future owner must drain/reset before ID wrap.
    uint64_t id = 0;
    uint64_t slots = 0;  // zero = inactive; remaining live layer slots otherwise
    Owner owner;
    uint8_t channels = 0;  // total remaining reservations, including release tails
    // Every remaining layer releases, or will be choked if this request admits.
    bool releasing = false;
};
struct Request {
    Owner owner;
    Policy policy;
    uint8_t slots = 0, channels = 0;  // cost of the entire resolved note/hit
};
enum class Result : uint8_t { Accepted, InvalidRequest, InvalidSnapshot, NoCapacity };
struct Plan {
    Result result = Result::InvalidRequest;
    uint64_t victims = 0;  // whole-group indices in the supplied snapshot
    uint64_t retire_slots = 0;
    uint64_t new_slots = 0;  // deterministic lowest available render slots
};

// Pure bounded planning used by the callback-owned VoiceManager. The caller
// must plan/commit without changing this snapshot
// in between. Rejection publishes no victims, slot assignments or choke effects.
template <size_t Slots = WAVEX_NUM_VOICES, size_t Channels = WAVEX_AUDIO_CHANNEL_BUDGET>
class Admission {
   public:
    static_assert(Slots > 0 && Slots <= 64 && Channels > 0 && Channels <= 64);
    using Snapshot = std::array<Group, Slots>;

    // Keep planning in the caller's execution region. On the M7 that is the
    // ITCM TriggerGroup path; QSPI planner fetches amplified measured peaks
    // during Pattern file operations. Storage and algorithm remain unchanged.
    [[gnu::always_inline]] static inline Plan Build(const Snapshot& groups,
                                                    const Request& request) {
        const auto& policy = request.policy;
        if (!ValidOwner(request.owner) || policy.limit > 8 ||
            static_cast<uint8_t>(policy.mode) > static_cast<uint8_t>(PlayMode::Mono) ||
            static_cast<uint8_t>(policy.steal) > static_cast<uint8_t>(StealFrom::Any) ||
            !request.slots || request.channels < request.slots ||
            request.channels > 2u * request.slots)
            return {};
        if (request.slots > Slots || request.channels > Channels)
            return {Result::NoCapacity};

        uint64_t occupied = 0;
        unsigned used_channels = 0, own_groups = 0;
        for (size_t i = 0; i < Slots; ++i) {
            const auto& g = groups[i];
            if (!g.slots)
                continue;
            const auto count = Count(g.slots);
            if (!g.id || !ValidOwner(g.owner) || (g.slots & ~AllSlots()) || (g.slots & occupied) ||
                g.channels < count || g.channels > 2u * count)
                return {Result::InvalidSnapshot};
            for (size_t j = 0; j < i; ++j)
                if (groups[j].slots && groups[j].id == g.id)
                    return {Result::InvalidSnapshot};
            occupied |= g.slots;
            used_channels += g.channels;
            own_groups += g.owner == request.owner;
        }
        if (used_channels > Channels)
            return {Result::InvalidSnapshot};

        Plan plan{Result::Accepted};
        const auto retire = [&](size_t i) __attribute__((always_inline)) {
            const auto& g = groups[i];
            plan.victims |= uint64_t{1} << i;
            plan.retire_slots |= g.slots;
            occupied &= ~g.slots;
            used_channels -= g.channels;
            own_groups -= g.owner == request.owner;
        };
        const unsigned cap = policy.mode == PlayMode::Mono ? 1 : policy.limit;
        // Local caps apply even with spare global channels. Foreign victims
        // cannot satisfy a cap on this Track binding.
        for (size_t n = 0; cap && own_groups >= cap && n < Slots; ++n) {
            const auto victim = Best(groups, plan.victims, request.owner, true);
            if (victim == Slots)
                return {Result::NoCapacity};
            retire(victim);
        }
        for (size_t n = 0; n < Slots && (Count(occupied) + request.slots > Slots ||
                                         used_channels + request.channels > Channels);
             ++n) {
            size_t victim = Slots;
            if (policy.steal != StealFrom::Any)
                victim = Best(groups, plan.victims, request.owner, true);
            if (victim == Slots && policy.steal != StealFrom::OwnOnly)
                victim = Best(groups, plan.victims, request.owner, false);
            if (victim == Slots)
                return {Result::NoCapacity};
            retire(victim);
        }
        if (Count(occupied) + request.slots > Slots ||
            used_channels + request.channels > Channels || (cap && own_groups >= cap))
            return {Result::NoCapacity};

        unsigned assigned = 0;
        for (size_t i = 0; i < Slots && assigned < request.slots; ++i) {
            const uint64_t bit = uint64_t{1} << i;
            if (!(occupied & bit)) {
                plan.new_slots |= bit;
                ++assigned;
            }
        }
        return plan;
    }

   private:
    static constexpr uint64_t AllSlots() {
        if constexpr (Slots == 64)
            return ~uint64_t{0};
        else
            return (uint64_t{1} << Slots) - 1;
    }
    static bool ValidOwner(Owner owner) { return owner.binding && owner.track < 16; }
    static unsigned Count(uint64_t bits) {
        unsigned count = 0;
        for (; bits; bits &= bits - 1)
            ++count;  // at most Slots iterations for a validated snapshot
        return count;
    }
    [[gnu::always_inline]] static inline size_t Best(const Snapshot& groups,
                                                     uint64_t victims,
                                                     Owner owner,
                                                     bool own_only) {
        size_t best = Slots;
        for (size_t i = 0; i < Slots; ++i) {
            const auto& g = groups[i];
            if (!g.slots || (victims & (uint64_t{1} << i)) || (own_only && !(g.owner == owner)))
                continue;
            if (best == Slots || (g.releasing && !groups[best].releasing) ||
                (g.releasing == groups[best].releasing && g.id < groups[best].id))
                best = i;
        }
        return best;
    }
};
}  // namespace WaveX::AudioEngine::Allocation
