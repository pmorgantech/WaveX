#pragma once
#include "ui/panel_assignments.h"

#include <array>

namespace wavex_ui {
static_assert(WAVEX_LED_CHANNELS == 48, "Two PCA9956B devices provide 48 channels");
struct PanelLedFrame {
    std::array<uint8_t, WAVEX_LED_CHANNELS> levels{};
    bool blank = true;
    bool operator==(const PanelLedFrame& rhs) const {
        return levels == rhs.levels && blank == rhs.blank;
    }
    bool operator!=(const PanelLedFrame& rhs) const { return !(*this == rhs); }
};
struct PanelLedInputs {
    RootGroup root = RootGroup::None;
    uint32_t available_roots = 0;
    uint32_t held_buttons = 0;
    uint8_t enabled_softkeys = 0;
    uint8_t active_softkeys = 0;
    bool blank = false;
};
// Only this initial set is electrically enabled. Other reserved outputs get
// both LEDOUT=off and IREF=0, including the as-yet-unfitted pads/transport.
constexpr std::array<bool, WAVEX_LED_CHANNELS> panelLedPopulation() {
    std::array<bool, WAVEX_LED_CHANNELS> used{};
    for (unsigned i = 0; i < 6; ++i)
        used[panelLedChannel(static_cast<PanelLed>(static_cast<unsigned>(PanelLed::Soft1) + i))] =
            true;
    used[panelLedChannel(PanelLed::Shift)] = true;
    for (const auto& item: kPanelMenuButtons)
        if (item.purpose)
            used[panelLedChannel(item.led)] = true;
    return used;
}
inline PanelLedFrame makePanelLedFrame(const PanelLedInputs& input) {
    PanelLedFrame frame;
    frame.blank = input.blank;
    if (input.blank)
        return frame;
    for (unsigned i = 0; i < 6; ++i) {
        if (!(input.enabled_softkeys & (1u << i)))
            continue;
        const bool active = (input.active_softkeys & (1u << i)) || (input.held_buttons & (1u << i));
        frame.levels[panelLedChannel(
            static_cast<PanelLed>(static_cast<unsigned>(PanelLed::Soft1) + i))] =
            active ? WAVEX_PANEL_LED_BRIGHT : WAVEX_PANEL_LED_DIM;
    }
    if (input.held_buttons & panelButtonBit(PanelKey::Shift))
        frame.levels[panelLedChannel(PanelLed::Shift)] = WAVEX_PANEL_LED_BRIGHT;
    for (const auto& item: kPanelMenuButtons)
        if (item.purpose && item.group == input.root &&
            (input.available_roots & (1u << static_cast<unsigned>(item.group))))
            frame.levels[panelLedChannel(item.led)] = WAVEX_PANEL_LED_BRIGHT;
    return frame;
}
}  // namespace wavex_ui
