#pragma once
#include "panel_led_state.h"
#include "ui/panel_led.h"
#include "ui/root_group.h"

#include <array>

namespace wavex_ui {
// Logical, chip-independent 8-bit brightness. Bus, PWM packing and channel
// wiring belong to the output backend, never to pages or LED policy.
struct PanelLedFrame {
    std::array<uint8_t, kPanelLedCount> levels{};
    bool blanked = true;
    int16_t test_channel = -1;  // diagnostic physical output, -1 = normal
    bool test_all = false;
    bool operator==(const PanelLedFrame& other) const {
        return levels == other.levels && blanked == other.blanked &&
               test_channel == other.test_channel && test_all == other.test_all;
    }
    bool operator!=(const PanelLedFrame& other) const { return !(*this == other); }
};
// The consumer darkens stale UI state, including a diagnostic override.
inline PanelLedFrame FreshPanelLedFrame(PanelLedFrame frame, uint32_t now, uint32_t published_at) {
    if (static_cast<uint32_t>(now - published_at) > 1000)
        frame.blanked = true;
    return frame;
}
struct PanelLedInputs {
    RootGroup root = RootGroup::None;
    bool shifted = false, blanked = false, playing = false;
    std::array<bool, 6> soft_defined{}, soft_enabled{}, soft_active{};
    PanelPageLeds pads;
};
inline PanelLedFrame BuildPanelLedFrame(const PanelLedInputs& input) {
    constexpr uint8_t dim = 32, bright = 192;
    PanelLedFrame frame;
    frame.blanked = input.blanked;
    if (input.blanked)
        return frame;
    auto set = [&](PanelLed led, uint8_t value) { frame.levels[static_cast<size_t>(led)] = value; };
    for (size_t i = 0; i < 6; ++i)
        frame.levels[i] = !input.soft_defined[i]                          ? 0
                          : input.soft_enabled[i] && input.soft_active[i] ? bright
                                                                          : dim;
    set(PanelLed::Shift, input.shifted ? bright : 0);
    const RootGroup groups[]{RootGroup::Sample,
                             RootGroup::Play,
                             RootGroup::Instrument,
                             RootGroup::Track,
                             RootGroup::Mixer,
                             RootGroup::Settings};
    for (size_t i = 0; i < 6; ++i)
        frame.levels[static_cast<size_t>(PanelLed::JumpSample) + i] =
            input.root == groups[i] ? bright : 0;
    set(PanelLed::PlayStop, input.playing ? bright : 0);
    set(PanelLed::Rec, input.pads.recording ? bright : 0);
    for (size_t i = 0; i < 16; ++i)
        frame.levels[static_cast<size_t>(PanelLed::Pad1) + i] =
            input.pads.active & (1u << i)    ? bright
            : input.pads.defined & (1u << i) ? dim
                                             : 0;
    return frame;
}
// Wiring translation is also useful to diagnostics, but policy above has no
// channel arithmetic. Unassigned physical outputs are always zero.
inline uint8_t PanelLedChannelLevel(const PanelLedFrame& frame, size_t channel) {
    if (frame.blanked || channel >= WAVEX_LED_CHANNELS)
        return 0;
    if (frame.test_all || frame.test_channel == static_cast<int16_t>(channel))
        return 255;
    if (frame.test_channel >= 0)
        return 0;
    for (size_t i = 0; i < kPanelLedCount; ++i)
        if (panelLedChannel(static_cast<PanelLed>(i)) == channel)
            return frame.levels[i];
    return 0;
}
}  // namespace wavex_ui
