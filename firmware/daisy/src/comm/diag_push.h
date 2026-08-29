#pragma once

#include <stdint.h>

namespace WaveX {
namespace Comm {

// Diagnostics telemetry push (MSG_DIAG_PUSH). See docs/ui-diagnostics-spec.md.
//
// Gated on a subscription so it costs exactly nothing while the frontend's
// diagnostics page is closed, which is nearly all of the time. Every counter
// it sends is a delta over the interval and is reset on read, so each push
// describes its own window rather than the whole run.

/** Applies a MSG_DIAG_SUBSCRIBE request. interval_hz is clamped to 1..10. */
void DiagSubscribe(bool enable, uint8_t interval_hz);

/** True while the frontend has an active subscription. */
bool DiagIsSubscribed();

/**
 * @brief Sends one push if the interval has elapsed. Cheap no-op otherwise.
 *
 * Call from the main loop, never the audio callback: it reads counters that
 * the callback writes, and it transmits.
 *
 * @param now_ms Current millisecond tick (same clock as the heartbeat's).
 */
void DiagPushTick(uint32_t now_ms);

}  // namespace Comm
}  // namespace WaveX
