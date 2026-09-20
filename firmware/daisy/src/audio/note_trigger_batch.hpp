#pragma once

#include "audio/note_group_admission.hpp"

namespace WaveX::AudioEngine {

// Admission metadata only: prepared sample/DSP parameters stay in their
// immutable map until the corresponding layer survives the batch. A batch
// has no intervening render, release, binding change or control update.
struct TriggerDescription {
    uint8_t track = 0, count = 0;
    struct Layer {
        uint8_t channels = 1, choke = 0;
    };
    std::array<Layer, WAVEX_NUM_VOICES> layers{};
    Allocation::Policy policy;
};

class NoteTriggerBatch {
   public:
    static constexpr uint16_t kExisting = UINT16_MAX;
    struct Slot {
        uint64_t id = 0;  // zero is free
        Allocation::Owner owner;
        uint16_t request = kExisting;
        uint8_t layer = 0, channels = 0, choke = 0;
        bool releasing = false;
    };
    std::array<Slot, WAVEX_NUM_VOICES> slots{};

    // Replays the ordinary whole-note admission and choke ordering. Rejected
    // notes cannot change the working state; superseded notes still consume
    // identities and RNG/age sequence positions in the caller.
    [[gnu::always_inline]] inline Allocation::Plan Admit(const TriggerDescription& note,
                                                         uint32_t binding,
                                                         uint64_t id,
                                                         uint16_t request) {
        if (!id || request == kExisting || !note.count || note.count > slots.size())
            return {};
        uint8_t channels = 0;
        for (uint8_t i = 0; i < note.count; ++i) {
            if (note.layers[i].channels < 1 || note.layers[i].channels > 2)
                return {};
            channels += note.layers[i].channels;
        }
        const auto chokes = [&](const Slot& slot) {
            if (!slot.choke || slot.owner.track != note.track)
                return false;
            for (uint8_t i = 0; i < note.count; ++i)
                if (note.layers[i].choke == slot.choke)
                    return true;
            return false;
        };
        Allocation::Admission<>::Snapshot groups{};
        for (size_t i = 0; i < slots.size(); ++i) {
            const auto& slot = slots[i];
            if (!slot.id)
                continue;
            size_t group = 0;
            while (group < i && groups[group].id != slot.id)
                ++group;
            auto& g = groups[group];
            if (!g.slots) {
                g.id = slot.id;
                g.owner = slot.owner;
                g.releasing = true;
            }
            g.slots |= uint64_t{1} << i;
            g.channels += slot.channels;
            g.releasing = g.releasing && (slot.releasing || chokes(slot));
        }
        const auto plan = Allocation::Admission<>::Build(
            groups, {{binding, note.track}, note.policy, note.count, channels});
        if (plan.result != Allocation::Result::Accepted)
            return plan;
        uint8_t layer = 0;
        for (size_t i = 0; i < slots.size(); ++i) {
            auto& slot = slots[i];
            if (plan.retire_slots & (uint64_t{1} << i))
                slot = {};
            if (slot.id && chokes(slot))
                slot.releasing = true;
            if (plan.new_slots & (uint64_t{1} << i)) {
                slot = {id,
                        {binding, note.track},
                        request,
                        layer,
                        note.layers[layer].channels,
                        note.layers[layer].choke,
                        false};
                ++layer;
            }
        }
        return plan;
    }
};
}  // namespace WaveX::AudioEngine
