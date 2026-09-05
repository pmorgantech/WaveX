// WaveX UI Softkey Definition
#pragma once

#include "ui/panel_key.h"

#include <array>
#include <cstdint>
#include <functional>
#include <string>

namespace wavex_ui {

struct Softkey {
    std::string label;              ///< Display text; empty renders an inert slot
    std::function<void()> onPress;  ///< Callback when button is pressed
    bool enabled = true;            ///< False renders it dimmed, not hidden - the
                                    ///< row is a fixed six positions and a key
                                    ///< that vanishes moves every other one
    std::string why{};              ///< Shown when disabled: why it does nothing.
                                    ///< The {} is load-bearing: an NSDMI keeps
                                    ///< -Wmissing-field-initializers quiet at the
                                    ///< dozens of {label, cb} aggregate inits.
};

constexpr int NUM_SOFTKEYS = 6;

/**
 * @brief The four bench buttons' ids, as carried in InputEvent::source_id.
 *
 * Aliases for the PanelKey values (panel_key.h) that replaced them; kept so
 * nothing that compares an id has to change in the same commit. Shift and
 * Back are intercepted globally by the InputDispatcher rather than handled
 * per page, so every screen gets the same modifier for free and no page can
 * accidentally swallow it.
 */
constexpr uint8_t BUTTON_SELECT = static_cast<uint8_t>(PanelKey::NavAPush);
constexpr uint8_t BUTTON_BACK = static_cast<uint8_t>(PanelKey::Back);
constexpr uint8_t BUTTON_ENCODER_CLICK = static_cast<uint8_t>(PanelKey::NavBPush);
constexpr uint8_t BUTTON_SHIFT = static_cast<uint8_t>(PanelKey::Shift);

}  // namespace wavex_ui
