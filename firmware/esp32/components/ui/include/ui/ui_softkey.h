// WaveX UI Softkey Definition
#pragma once

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
 * @brief Logical button ids carried in InputEvent::source_id.
 *
 * Shift is intercepted globally by the InputDispatcher rather than handled
 * per page, so every screen gets the same modifier for free and no page can
 * accidentally swallow it.
 */
constexpr uint8_t BUTTON_SELECT = 1;
constexpr uint8_t BUTTON_BACK = 2;
constexpr uint8_t BUTTON_ENCODER_CLICK = 3;
constexpr uint8_t BUTTON_SHIFT = 4;

}  // namespace wavex_ui
