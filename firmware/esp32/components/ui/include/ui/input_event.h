// WaveX Unified Input Events
#pragma once

#include <cstdint>

namespace wavex_ui {

enum class InputType : uint8_t {
    TouchDown,
    TouchUp,
    TouchMove,
    ButtonPress,
    ButtonRelease,
    EncoderLeft,
    EncoderRight,
    EncoderClick,
    EncoderUp,
    EncoderDown
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
                return magnitude;
            case InputType::EncoderLeft:
            case InputType::EncoderDown:
                return -magnitude;
            default:
                return 0;
        }
    }
};

}  // namespace wavex_ui
