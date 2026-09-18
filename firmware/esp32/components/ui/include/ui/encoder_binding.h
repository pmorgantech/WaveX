#pragma once
#include <array>
#include <cstdint>
namespace wavex_ui {
struct EncoderBinding {
    const char* label = "";
    std::array<char, 32> value{};
    void* owner = nullptr;
    void (*onSteps)(void*, uint8_t, int) = nullptr;
    uint8_t parameter = 0;
    uint8_t coarse_step = 4;  // callback units per normal step; Shift uses one
    bool enabled = false;
};
using EncoderBindings = std::array<EncoderBinding, 4>;
inline bool InvokeEncoder(const EncoderBindings& bindings, uint8_t index, int steps, bool fine) {
    if (index >= bindings.size() || !steps)
        return false;
    const auto& binding = bindings[index];
    if (!binding.enabled || !binding.owner || !binding.onSteps)
        return false;
    binding.onSteps(binding.owner, binding.parameter, steps * (fine ? 1 : binding.coarse_step));
    return true;
}
}  // namespace wavex_ui
