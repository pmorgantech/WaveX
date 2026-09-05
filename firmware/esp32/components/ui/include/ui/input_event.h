// WaveX Unified Input Events
#pragma once

#include "ui/panel_key.h"

#include <cstdint>

namespace wavex_ui {

enum class InputType : uint8_t {
    TouchDown,
    TouchUp,
    TouchMove,
    ButtonPress,  ///< A panel key with Select semantics; source_id is its PanelKey.
    ButtonRelease,
    EncoderLeft,
    EncoderRight,
    EncoderClick,
    EncoderUp,
    EncoderDown,
    /**
     * A panel key, any key: source_id is the PanelKey. The keypad task posts
     * these for every key it decodes; InputDispatcher::processAll() acts on
     * the global ones (Shift, Back, softkeys, jumps, Track -/+) and forwards
     * the rest. The two Select keys arrive at a page re-typed as
     * ButtonPress/ButtonRelease, which is what pages have always read as
     * "activate" - so a page that never learned PanelKey still works, and a
     * key it does not know cannot be mistaken for Select.
     */
    KeyPress,
    KeyRelease,
    /**
     * An endless pot (2.P.4): source_id is the pot index 0..3, delta the
     * magnitude, direction in the type - the contract steps() enforces.
     */
    PotUp,
    PotDown
};

struct InputEvent {
    InputType type;
    uint8_t source_id = 0;

    /**
     * How far the control moved, as a MAGNITUDE - never signed.
     *
     * Direction lives in `type`, and only in `type`. The two producers used to
     * disagree about this: the rotary encoder posted its raw signed count while
     * the pot posted a magnitude, so a page negating `delta` on an EncoderLeft
     * flipped it back to positive and turned counter-clockwise into an
     * increase. That shipped on the sample edit page, and was still live on the
     * voice page after the edit page patched it locally.
     *
     * Read it through steps() rather than directly.
     */
    int16_t delta = 0;

    uint16_t x = 0;
    uint16_t y = 0;
    uint32_t timestamp_ms = 0;

    /**
     * @brief Signed step count: size from `delta`, direction from `type`.
     *
     * Clockwise - EncoderRight, or EncoderUp from the pot - is positive,
     * always. That is a global contract, not a per-page choice (roadmap 1.5.2
     * item 5).
     *
     * Returns 0 for non-encoder events, and at least one step for an encoder
     * event carrying no magnitude, so a producer that forgets to set `delta`
     * still moves the control by one rather than not at all.
     *
     * The magnitude is taken defensively even though both producers now post
     * one. A page written against this helper cannot be inverted by a
     * regression on the other side of the queue, which is the whole reason it
     * exists rather than each page repeating the reasoning.
     */
    int steps() const {
        const int magnitude = (delta > 0) ? delta : (delta < 0 ? -delta : 1);
        switch (type) {
            case InputType::EncoderRight:
            case InputType::EncoderUp:
            case InputType::PotUp:
                return magnitude;
            case InputType::EncoderLeft:
            case InputType::EncoderDown:
            case InputType::PotDown:
                return -magnitude;
            default:
                return 0;
        }
    }

    /// The panel key behind a key or button event; PanelKey::None otherwise.
    PanelKey key() const {
        switch (type) {
            case InputType::KeyPress:
            case InputType::KeyRelease:
            case InputType::ButtonPress:
            case InputType::ButtonRelease:
                return source_id < static_cast<uint8_t>(PanelKey::Count)
                           ? static_cast<PanelKey>(source_id)
                           : PanelKey::None;
            default:
                return PanelKey::None;
        }
    }

    bool isKeyPress() const {
        return type == InputType::KeyPress || type == InputType::ButtonPress;
    }
};

}  // namespace wavex_ui
