#pragma once

#include <array>
#include <cstdint>

namespace wavex_ui {

// Checked, HAL-free register protocol. Registers supplies bool read/write;
// Sink receives complete (pressed, keycode) events. One keypad-task owner.
// TI TCA8418 datasheet sections 8.3.1.3, 8.3.5.1 and 8.6.2.
class Tca8418Controller {
   public:
    enum class State { Idle, Pending, IoError, InvalidEvent };
    struct Result {
        State state = State::Idle;
        uint8_t events = 0;
        bool overflow = false;
    };
    static constexpr uint8_t kMaxEventsPerPass = 16;

    template <typename Registers>
    bool init(Registers& bus, uint8_t rows, uint8_t columns) {
        if (!rows || rows > 8 || !columns || columns > 10)
            return false;
        held_.fill(false);
        // Disable host interrupts while configuring. Unused pins are inputs,
        // excluded from the event FIFO and from GPI interrupt generation.
        if (!bus.write(0x01, 0))
            return false;
        for (uint8_t base: std::array<uint8_t, 5>{0x1A, 0x20, 0x23, 0x26, 0x29})
            for (uint8_t i = 0; i < 3; ++i)
                if (!bus.write(static_cast<uint8_t>(base + i), 0))
                    return false;
        return bus.write(0x1D, static_cast<uint8_t>((1u << rows) - 1)) &&
               bus.write(0x1E, static_cast<uint8_t>((1u << (columns < 8 ? columns : 8)) - 1)) &&
               bus.write(0x1F, columns > 8 ? static_cast<uint8_t>((1u << (columns - 8)) - 1) : 0);
    }

    template <typename Registers>
    bool interrupts(Registers& bus, bool enabled) {
        // KE_IEN + OVR_FLOW_IEN + INT_CFG: reassert an edge if an event
        // arrives while acknowledging. Do not overwrite old FIFO entries.
        const uint8_t cfg = enabled ? 0x19 : 0;
        uint8_t readback = 0;
        return bus.write(0x01, cfg) && bus.read(0x01, readback) && readback == cfg;
    }

    template <typename Registers, typename Sink>
    Result service(Registers& bus, Sink&& sink) {
        Result result;
        auto finish = [&](State state) {
            result.state = state;
            // A lost release must not leave a physical key held indefinitely.
            // Resynchronization releases only keys this owner has reported.
            if (result.overflow || state == State::IoError || state == State::InvalidEvent)
                releaseHeld(sink);
            return result;
        };
        uint8_t status = 0;
        if (!bus.read(0x02, status))
            return finish(State::IoError);
        result.overflow = (status & 0x08) != 0;
        for (;;) {
            uint8_t count = 0;
            if (!bus.read(0x03, count))
                return finish(State::IoError);
            count &= 0x0F;
            if (count > 10)
                return finish(State::InvalidEvent);
            if (!count)
                break;
            if (result.events == kMaxEventsPerPass)
                return finish(State::Pending);
            uint8_t event = 0;
            if (!bus.read(0x04, event))
                return finish(State::IoError);
            const uint8_t key = event & 0x7F;
            if (!key || key > 80)
                return finish(State::InvalidEvent);
            const bool pressed = (event & 0x80) != 0;
            held_[key - 1] = pressed;
            sink(pressed, key);
            ++result.events;
        }
        // Acknowledge only observed key/overflow flags, never flush the FIFO.
        // Include overflow that arose while draining, then check for events
        // racing the acknowledge even if the GPIO edge was missed.
        uint8_t after = 0;
        if (!bus.read(0x02, after))
            return finish(State::IoError);
        status |= after;
        result.overflow = (status & 0x08) != 0;
        if ((status & 0x09) && !bus.write(0x02, status & 0x09))
            return finish(State::IoError);
        uint8_t remaining = 0;
        if (!bus.read(0x03, remaining))
            return finish(State::IoError);
        if ((remaining & 0x0F) > 10)
            return finish(State::InvalidEvent);
        return finish((remaining & 0x0F) ? State::Pending : State::Idle);
    }

    template <typename Sink>
    void releaseHeld(Sink&& sink) {
        for (uint8_t i = 0; i < held_.size(); ++i) {
            if (held_[i]) {
                sink(false, static_cast<uint8_t>(i + 1));
                held_[i] = false;
            }
        }
    }

   private:
    std::array<bool, 80> held_{};
};

}  // namespace wavex_ui
