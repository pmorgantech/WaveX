#pragma once
#include <array>
#include <cstdint>

namespace wavex_ui {

// TCA8418 register/FIFO policy, independent of I2C and FreeRTOS. One task
// owns it. IO exposes read(reg, value) / write(reg, value), both return bool.
class KeypadFifo {
   public:
    enum class Result { Idle, More, IoError };
    static constexpr uint8_t kConfig =
        0x39;  // KE_IEN, INT_CFG, overflow enable + mode (TI erratum)

    template <class IO>
    bool configure(IO& io, uint8_t rows, uint8_t columns) {
        if (!rows || rows > 8 || !columns || columns > 10)
            return false;
        if (!io.write(0x01, 0))
            return false;
        // Disable GPIO events/interrupts; only keypad matrix transitions matter.
        for (uint8_t reg = 0x1a; reg <= 0x1c; ++reg)
            if (!io.write(reg, 0))
                return false;
        for (uint8_t reg = 0x20; reg <= 0x25; ++reg)
            if (!io.write(reg, 0))
                return false;
        for (uint8_t reg = 0x29; reg <= 0x2b; ++reg)
            if (!io.write(reg, 0))
                return false;  // debounce enabled
        const uint16_t cols = static_cast<uint16_t>((1u << columns) - 1u);
        if (!io.write(0x1d, static_cast<uint8_t>((1u << rows) - 1u)) ||
            !io.write(0x1e, static_cast<uint8_t>(cols)) ||
            !io.write(0x1f, static_cast<uint8_t>(cols >> 8)))
            return false;
        uint8_t config = 0;
        return io.write(0x01, kConfig) && io.read(0x01, config) && config == kConfig;
    }

    // Retain a rejected event until the UI queue has room. Never consume the
    // following release while its press is still pending.
    template <class IO, class Emit>
    Result service(IO& io, Emit emit) {
        if (!releaseHeld(emit))
            return Result::More;
        uint8_t status = 0;
        if (!io.read(0x02, status))
            return failed();
        if (status & 0x08) {
            ++overflows_;
            recovering_ = true;
            pending_ = 0;
            if (!releaseHeld(emit))
                return Result::More;
        }
        if (pending_ && !deliver(emit))
            return Result::More;
        for (unsigned i = 0; i < 16; ++i) {
            uint8_t count = 0, event = 0;
            if (!io.read(0x03, count))
                return failed();
            if (!(count & 0x0f)) {
                // Clear only observed keypad/overflow causes. INT_CFG creates
                // another edge if an event races this ACK; the count recheck
                // also catches it without depending on that edge.
                if (!io.write(0x02, status & 0x09) || !io.read(0x03, count))
                    return failed();
                if (!(count & 0x0f))
                    recovering_ = false;
                return (count & 0x0f) ? Result::More : Result::Idle;
            }
            if (!io.read(0x04, event))
                return failed();
            if (!event)
                return Result::More;  // inconsistent device, bounded retry
            if (recovering_)
                continue;  // history after overflow/error is ambiguous
            const uint8_t key = event & 0x7f;
            if (!key || key > 80) {
                ++invalid_;
                continue;
            }
            pending_ = event;
            if (!deliver(emit))
                return Result::More;
        }
        return Result::More;
    }

    template <class Emit>
    bool stop(Emit emit) {
        recovering_ = true;
        pending_ = 0;
        return releaseHeld(emit);
    }
    uint32_t errors() const { return errors_; }
    uint32_t overflows() const { return overflows_; }
    uint32_t invalid() const { return invalid_; }

   private:
    Result failed() {
        ++errors_;
        recovering_ = true;
        pending_ = 0;
        return Result::IoError;
    }
    template <class Emit>
    bool releaseHeld(Emit emit) {
        if (!recovering_)
            return true;
        for (uint8_t key = 1; key <= 80; ++key) {
            if (held_[key - 1]) {
                if (!emit(false, key))
                    return false;
                held_[key - 1] = false;
            }
        }
        return true;
    }
    template <class Emit>
    bool deliver(Emit emit) {
        const uint8_t key = pending_ & 0x7f;
        const bool pressed = (pending_ & 0x80) != 0;
        if (held_[key - 1] != pressed) {
            if (!emit(pressed, key))
                return false;
            held_[key - 1] = pressed;
        }
        pending_ = 0;
        return true;
    }
    std::array<bool, 80> held_{};
    uint8_t pending_ = 0;
    bool recovering_ = false;
    uint32_t errors_ = 0, overflows_ = 0, invalid_ = 0;
};
}  // namespace wavex_ui
