// WaveX root groups: the top level of the navigation structure
#pragma once

#include <cstdint>

namespace wavex_ui {

/**
 * @brief The groups directly under the main menu (ui-information-architecture.md §1).
 *
 * One entry per menu item and per panel jump key. A jump
 * (UINavigator::jumpToRoot) unwinds to the main menu and pushes the group's
 * page; the navigator remembers which, and that is what the panel's jump
 * LEDs show. The Track key opens Project; the Mixer key is reserved for
 * the full strip view: a jump to a group with no page registered
 * is refused, and the key still shows on the Diagnostics ▸ Panel tab.
 */
enum class RootGroup : uint8_t {
    None = 0,  ///< at the main menu
    Sample,
    Play,
    Instrument,
    Track,
    Mixer,
    Settings,
    Diagnostics,  ///< menu only; no panel key
    Sequencer,    ///< touchscreen composition surface
    Count
};

inline const char* rootGroupName(RootGroup group) {
    switch (group) {
        case RootGroup::None:
            return "-";
        case RootGroup::Sample:
            return "Sample";
        case RootGroup::Play:
            return "Play";
        case RootGroup::Instrument:
            return "Instrument";
        case RootGroup::Track:
            return "Project";
        case RootGroup::Mixer:
            return "Mixer";
        case RootGroup::Settings:
            return "Settings";
        case RootGroup::Sequencer:
            return "Sequencer";
        case RootGroup::Diagnostics:
            return "Diagnostics";
        default:
            return "?";
    }
}

}  // namespace wavex_ui
