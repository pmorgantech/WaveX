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
    int16_t delta = 0;
    uint16_t x = 0;
    uint16_t y = 0;
    uint32_t timestamp_ms = 0;
};

} // namespace wavex_ui


