#pragma once

// Non-blocking log transport for the ESP32, mirroring the Daisy's
// firmware/daisy/src/comm/log_ring.
//
// ESP-IDF console logging is synchronous: every ESP_LOGx formats and then
// busy-waits the whole line out of the UART0 FIFO at 115200 baud (~87 us per
// character) in the calling task. A burst of a dozen INFO lines therefore
// stalls its caller for ~100 ms - and the callers here include the UI task
// (while holding the LVGL lock) and the UART/SPI link tasks. That stall was
// the dominant cause of sluggish Sample Browser scrolling.
//
// This replaces the esp_log vprintf sink with the usual arrangement: writers
// format on their own stack, append to a ring, and never block; a
// lowest-priority drain task feeds the console a bounded chunk per pass. If
// output is produced faster than 115200 baud can carry it, the OLDEST
// buffered bytes are dropped (the newest output is normally the interesting
// part) and the loss is counted.
//
// Scope and caveats:
// - Only esp_log output (ESP_LOGx) is rerouted. Direct printf/fwrite to
//   stdout - e.g. the debug screenshot dump - keeps its synchronous path and
//   its ordering guarantees on its own task.
// - Panic/abort output bypasses the vprintf hook entirely, so crash dumps
//   still reach the console; up to a ring's worth of buffered lines from
//   before the panic may be lost.
// - Lines longer than 255 bytes are truncated.

#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Installs the vprintf hook and starts the drain task. Call once, early in
// app initialization (after the scheduler is running). Log calls made before
// this keep the default blocking behavior.
esp_err_t wavex_log_ring_install(void);

// Bytes discarded through overflow since boot. Non-zero means log volume
// exceeded the console baud rate and the log has gaps (each gap is also
// marked inline in the output).
uint32_t wavex_log_ring_dropped_bytes(void);

#ifdef __cplusplus
}
#endif
