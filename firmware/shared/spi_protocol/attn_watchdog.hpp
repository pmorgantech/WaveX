#pragma once

// ATTN-stuck-high recovery (roadmap Phase 1 item 7). The ESP32 asserts
// ATTN (GPIO, architecture.md §7.3 physical link) to tell the Daisy "I have
// data queued, please clock a transaction," and clears it in the SPI
// post-transaction callback once that transaction completes
// (esp_spi_link.cpp's spi_post_trans_cb, gated on trans->user meaning "this
// transaction carried a real message"). Today there is no timeout on that
// callback ever firing: if the Daisy never clocks the transaction (it
// rebooted, its EXTI handler missed the edge, its SPI peripheral wedged),
// ATTN stays asserted forever and the link never recovers without a manual
// ESP32 restart - the same class of bug as the UART-era hang
// (docs/archive/esp32-restart-hang-fix.md), just on the ATTN line instead
// of a blocking transmit call.
//
// AttnWatchdog is the same policy as that fix's approach (bound the stuck
// state with a timeout, force-clear past it) applied here: the caller marks
// ATTN asserted/cleared as it actually happens, and periodically asks
// ShouldForceDeassert() whether enough time has passed that the assertion
// should be considered abandoned and forcibly cleared.
//
// HAL-free: the caller supplies `now_ms` (from whatever real clock it has -
// esp_timer_get_time()/1000 on the ESP32), so this is host-testable with
// synthetic timestamps.

#include <cstdint>

namespace WaveX {
namespace Protocol {

class AttnWatchdog {
   public:
    // 500ms mirrors the "warn" threshold already used for the analogous
    // UART TX-stuck recovery (daisy_uart_link.cpp) - long enough that a
    // normal SPI transaction (electrically ~microseconds, worst-case
    // main-loop scheduling jitter in the low tens of ms) never trips it,
    // short enough that a genuinely wedged link recovers well within a
    // second rather than needing a manual restart.
    static constexpr uint32_t kForceDeassertMs = 500;

    // No-op if already asserted (doesn't reset the clock on repeated calls
    // - only the first assertion in a stuck streak should start the timer).
    void MarkAsserted(uint32_t now_ms) {
        if (!asserted_) {
            asserted_ = true;
            asserted_at_ms_ = now_ms;
        }
    }

    void MarkCleared() { asserted_ = false; }

    bool IsAsserted() const { return asserted_; }

    // True once ATTN has been asserted for at least kForceDeassertMs
    // without being cleared - the caller should force gpio_set_level(...,
    // 0), log the abandonment, and call MarkCleared().
    bool ShouldForceDeassert(uint32_t now_ms) const {
        if (!asserted_)
            return false;
        return (now_ms - asserted_at_ms_) >= kForceDeassertMs;
    }

   private:
    bool asserted_ = false;
    uint32_t asserted_at_ms_ = 0;
};

}  // namespace Protocol
}  // namespace WaveX
