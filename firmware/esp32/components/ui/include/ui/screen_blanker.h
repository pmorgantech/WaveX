// Fixed inactivity policy for the panel backlight.  Kept HAL-free so the
// wrap-safe timeout behaviour is host-testable independently of the BSP.
#pragma once

#include <cstdint>

namespace wavex_ui {

class ScreenBlanker {
   public:
    static constexpr uint32_t kTimeoutMs = 5u * 60u * 1000u;

    void Init(uint32_t now_ms) {
        last_activity_ms_ = now_ms;
        blanked_ = false;
    }

    void RecordActivity(uint32_t now_ms) { last_activity_ms_ = now_ms; }

    // Returns true once when the inactivity timeout expires. Unsigned
    // subtraction deliberately makes the ESP timer's 32-bit millisecond
    // wrap harmless.
    bool ShouldBlank(uint32_t now_ms) {
        if (blanked_ || static_cast<uint32_t>(now_ms - last_activity_ms_) < kTimeoutMs)
            return false;
        blanked_ = true;
        return true;
    }

    // Returns true only when an interaction wakes a blanked screen.
    bool ShouldWake(uint32_t now_ms) {
        const bool wake = blanked_;
        last_activity_ms_ = now_ms;
        blanked_ = false;
        return wake;
    }

    bool blanked() const { return blanked_; }

   private:
    uint32_t last_activity_ms_ = 0;
    bool blanked_ = false;
};

}  // namespace wavex_ui
