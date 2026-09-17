#pragma once

#include "ui/panel_key.h"
#include "ui/panel_led.h"
#include "ui/root_group.h"

#include <array>

namespace wavex_ui {
// Central logical assignment: main menu, physical jump keys and LEDs consume
// this same table. Physical keycodes/channels remain in hardware_config.h.
struct PanelMenuButton {
    RootGroup group;
    PanelKey key;
    PanelLed led;
    const char* purpose;  // nullptr = reserved, not a main-menu item yet
};
inline constexpr std::array<PanelMenuButton, 8> kPanelMenuButtons{{
    {RootGroup::Sample,
     PanelKey::JumpSample,
     PanelLed::JumpSample,
     "Manage / Browse / Edit / Record"},
    {RootGroup::Track,
     PanelKey::JumpTrack,
     PanelLed::JumpTrack,
     "Tracks / Instruments / Mixer / MIDI"},
    {RootGroup::Instrument,
     PanelKey::JumpInstrument,
     PanelLed::JumpInstrument,
     "Sample / Env / Amp / Filter / Mod"},
    {RootGroup::Play, PanelKey::JumpPlay, PanelLed::JumpPlay, "Pads / Keys"},
    {RootGroup::Sequencer,
     PanelKey::JumpSequencer,
     PanelLed::JumpSequencer,
     "Steps / Tempo / Swing"},
    {RootGroup::Settings,
     PanelKey::JumpSettings,
     PanelLed::JumpSettings,
     "Display / Storage / MIDI / System / Calibrate"},
    {RootGroup::Diagnostics,
     PanelKey::JumpDiagnostics,
     PanelLed::JumpDiagnostics,
     "ESP32 / Daisy / Audio / Link / Storage / MIDI / Panel"},
    {RootGroup::Mixer, PanelKey::JumpMixer, PanelLed::JumpMixer, nullptr},
}};
constexpr RootGroup panelMenuGroup(PanelKey key) {
    for (const auto& item: kPanelMenuButtons)
        if (item.key == key)
            return item.group;
    return RootGroup::None;
}
// One 32-bit input snapshot, safe to publish atomically on the 32-bit MCU.
constexpr uint32_t panelButtonBit(PanelKey key) {
    if (key >= PanelKey::Soft1 && key <= PanelKey::Soft6)
        return 1u << (static_cast<unsigned>(key) - static_cast<unsigned>(PanelKey::Soft1));
    if (key == PanelKey::Shift)
        return 1u << 6;
    for (unsigned i = 0; i < kPanelMenuButtons.size(); ++i)
        if (kPanelMenuButtons[i].key == key)
            return 1u << (7 + i);
    return 0;
}
constexpr bool validPanelAssignments() {
    for (size_t i = 0; i < kPanelMenuButtons.size(); ++i)
        for (size_t j = 0; j < i; ++j)
            if (kPanelMenuButtons[i].key == kPanelMenuButtons[j].key ||
                kPanelMenuButtons[i].led == kPanelMenuButtons[j].led ||
                kPanelMenuButtons[i].group == kPanelMenuButtons[j].group)
                return false;
    return true;
}
static_assert(validPanelAssignments(), "Menu button/LED assignments must be unique");
}  // namespace wavex_ui
