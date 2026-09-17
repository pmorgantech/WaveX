#pragma once

#include "ui/panel_led_model.h"

namespace wavex_ui {
// Allocation-free register sequencer. The panel task owns the backend. Each
// service call performs at most one bounded I2C transaction, outside LVGL.
class Pca9956bController {
   public:
    enum class State : uint8_t {
        Off,
        NeedsCurrent,
        ResetLow,
        ResetWait,
        Setup,
        Ready,
        Frame,
        Retry
    };
    explicit Pca9956bController(unsigned devices = WAVEX_PCA9956B_POPULATED_DEVICES,
                                uint8_t current = WAVEX_PCA9956B_IREF,
                                std::array<bool, WAVEX_LED_CHANNELS> used = panelLedPopulation())
        : devices_(devices), current_(current), used_(used) {}
    State state() const { return state_; }
    uint32_t errors() const { return errors_; }
    uint32_t frames() const { return frames_; }
    uint8_t faultStatus() const { return fault_status_; }

    template <class Bus>
    void stop(Bus& bus) {
        bus.blank(true);
        bus.reset(false);
        state_ = State::Off;
        applied_valid_ = false;
    }

    template <class Bus>
    void service(Bus& bus, const PanelLedFrame& desired, uint32_t now) {
        if (desired.blank && !bus.blank(true)) {
            fail(bus, now);
            return;
        }
        if (state_ == State::Off) {
            if (!bus.blank(true)) {
                fail(bus, now);
                return;
            }
            if (!current_ || devices_ == 0 || devices_ > WAVEX_PCA9956B_DEVICE_COUNT) {
                state_ = State::NeedsCurrent;
                return;
            }
            if (!bus.reset(true)) {
                fail(bus, now);
                return;
            }
            deadline_ = now + 1;
            state_ = State::ResetLow;
            return;
        }
        if (state_ == State::NeedsCurrent)
            return;
        if (state_ == State::Retry) {
            if (due(now, deadline_))
                state_ = State::Off;
            return;
        }
        if (state_ == State::ResetLow) {
            if (!due(now, deadline_))
                return;
            if (!bus.reset(false)) {
                fail(bus, now);
                return;
            }
            deadline_ = now + 2;
            state_ = State::ResetWait;
            return;
        }
        if (state_ == State::ResetWait) {
            if (!due(now, deadline_))
                return;
            device_ = phase_ = 0;
            state_ = State::Setup;
        }
        if (state_ == State::Setup) {
            std::array<uint8_t, 24> bytes{};
            bool ok = false;
            switch (phase_) {
                case 0:  // No all-call/subaddress response and no sleep; AI range = all registers.
                    ok = bus.write(device_, 0x00, bytes.data(), 1);
                    break;
                case 1:  // Clear errors; update outputs on STOP, individual dimming.
                    bytes[0] = 0x10;
                    ok = bus.write(device_, 0x01, bytes.data(), 1);
                    break;
                case 2:
                    ok = bus.write(device_, 0x0A, bytes.data(), 24);
                    break;
                case 3:
                    for (unsigned i = 0; i < 24; ++i)
                        bytes[i] = used_[device_ * 24 + i] ? current_ : 0;
                    ok = bus.write(device_, 0x22, bytes.data(), 24);
                    break;
                case 4:
                    for (unsigned i = 0; i < 24; ++i)
                        if (used_[device_ * 24 + i])
                            bytes[i / 4] |= static_cast<uint8_t>(2u << (2 * (i % 4)));
                    ok = bus.write(device_, 0x02, bytes.data(), 6);
                    break;
                default:
                    ok = healthy(bus, device_);
                    break;
            }
            if (!ok) {
                fail(bus, now);
                return;
            }
            if (++phase_ == 6) {
                phase_ = 0;
                if (++device_ == devices_) {
                    device_ = 0;
                    applied_valid_ = false;
                    last_refresh_ = now - WAVEX_PANEL_LED_REFRESH_MS;
                    last_health_ = now;
                    state_ = State::Ready;
                }
            }
            return;
        }
        if (state_ == State::Frame) {
            if (!bus.write(device_, 0x0A, pending_.levels.data() + device_ * 24, 24)) {
                fail(bus, now);
                return;
            }
            if (++device_ == devices_) {
                applied_ = pending_;
                applied_valid_ = true;
                ++frames_;
                last_refresh_ = now;
                state_ = State::Ready;
                // Never expose an obsolete frame after blanking or startup.
                if (!desired.blank && sanitize(desired) == applied_ && !bus.blank(false))
                    fail(bus, now);
            }
            return;
        }
        if (state_ == State::Ready) {
            if (now - last_health_ >= WAVEX_PANEL_LED_RETRY_MS) {
                if (!healthy(bus, health_device_)) {
                    fail(bus, now);
                    return;
                }
                if (++health_device_ == devices_) {
                    health_device_ = 0;
                    last_health_ = now;
                }
                return;
            }
            const PanelLedFrame next = sanitize(desired);
            if (applied_valid_ && next == applied_ && !desired.blank && !bus.blank(false)) {
                fail(bus, now);
                return;
            }
            if ((!applied_valid_ || next != applied_) &&
                now - last_refresh_ >= WAVEX_PANEL_LED_REFRESH_MS) {
                pending_ = next;
                device_ = 0;
                state_ = State::Frame;
            }
        }
    }

   private:
    static bool due(uint32_t now, uint32_t deadline) {
        return static_cast<int32_t>(now - deadline) >= 0;
    }
    PanelLedFrame sanitize(PanelLedFrame frame) const {
        for (unsigned i = 0; i < WAVEX_LED_CHANNELS; ++i)
            if (frame.blank || !used_[i] || i >= devices_ * 24)
                frame.levels[i] = 0;
        return frame;
    }
    template <class Bus>
    bool healthy(Bus& bus, unsigned device) {
        uint8_t mode[2]{};
        if (!bus.read(device, 0x00, mode, 2))
            return false;
        fault_status_ = static_cast<uint8_t>(mode[1] & 0xC0);
        // AIF is read-only. MODE2 bit 2 reads one; CLRERR self-clears.
        return (mode[0] & 0x7F) == 0 && (mode[1] & 0xE8) == 0;
    }
    template <class Bus>
    void fail(Bus& bus, uint32_t now) {
        bus.blank(true);
        ++errors_;
        applied_valid_ = false;
        health_device_ = 0;
        deadline_ = now + WAVEX_PANEL_LED_RETRY_MS;
        state_ = State::Retry;
    }
    unsigned devices_, device_ = 0, phase_ = 0, health_device_ = 0;
    uint8_t current_, fault_status_ = 0;
    std::array<bool, WAVEX_LED_CHANNELS> used_;
    State state_ = State::Off;
    uint32_t deadline_ = 0, last_refresh_ = 0, last_health_ = 0, errors_ = 0, frames_ = 0;
    bool applied_valid_ = false;
    PanelLedFrame pending_{}, applied_{};
};
inline const char* panelLedStateName(Pca9956bController::State state) {
    switch (state) {
        case Pca9956bController::State::Off:
            return "off";
        case Pca9956bController::State::NeedsCurrent:
            return "current-unset";
        case Pca9956bController::State::ResetLow:
        case Pca9956bController::State::ResetWait:
        case Pca9956bController::State::Setup:
            return "starting";
        case Pca9956bController::State::Ready:
        case Pca9956bController::State::Frame:
            return "ready";
        case Pca9956bController::State::Retry:
            return "retry";
    }
    return "unknown";
}
}  // namespace wavex_ui
