#include "log_ring.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace {

// 8 KiB absorbs the burstiest observed case (directory-load logging plus a
// wall of link telemetry) and is irrelevant against PSRAM-backed heap sizes.
// Must be a power of two only by convention here - the arithmetic below uses
// modulo, not masking.
constexpr size_t kRingBytes = 8192;

// Per-line formatting bound. ESP_LOG lines beyond this are truncated; the
// Daisy implementation uses the same figure and it has never clipped anything
// anyone missed.
constexpr size_t kLineBytes = 256;

// Bytes fed to the console per drain pass. The console write busy-waits
// (~87 us/byte at 115200), so this bounds how long the drain task spins
// before sleeping again: 256 bytes ~= 22 ms of spin per 20 ms sleep, which
// keeps the idle task (and its watchdog) comfortably fed while sustaining
// roughly the UART line rate.
constexpr size_t kChunkBytes = 256;
constexpr TickType_t kDrainPeriod = pdMS_TO_TICKS(20);

char s_ring[kRingBytes];
size_t s_head = 0;  // write position
size_t s_tail = 0;  // read position
uint32_t s_dropped = 0;
uint32_t s_dropped_reported = 0;

// Writers run on both cores; a spinlock is the correct cross-core guard and
// the held section is a bounded memcpy.
portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;

size_t buffered_locked() {
    return (s_head + kRingBytes - s_tail) % kRingBytes;
}

// Appends bytes, discarding the oldest buffered output to make room rather
// than blocking or refusing the write. Caller must hold s_lock.
void write_locked(const char* data, size_t len) {
    if (len >= kRingBytes) {
        const size_t excess = len - (kRingBytes - 1);
        data += excess;
        len -= excess;
        s_dropped += excess;
    }

    const size_t free_bytes = (kRingBytes - 1) - buffered_locked();
    if (len > free_bytes) {
        const size_t discard = len - free_bytes;
        s_tail = (s_tail + discard) % kRingBytes;
        s_dropped += discard;
    }

    const size_t first = (kRingBytes - s_head < len) ? (kRingBytes - s_head) : len;
    memcpy(&s_ring[s_head], data, first);
    if (len > first) {
        memcpy(&s_ring[0], data + first, len - first);
    }
    s_head = (s_head + len) % kRingBytes;
}

// esp_log sink. Formatting happens on the caller's stack; only the result
// touches the ring, under the spinlock. Never blocks.
int ring_vprintf(const char* fmt, va_list args) {
    char buf[kLineBytes];
    const int n = vsnprintf(buf, sizeof(buf), fmt, args);
    if (n <= 0) {
        return n;
    }
    const size_t len =
        (static_cast<size_t>(n) >= sizeof(buf)) ? sizeof(buf) - 1 : static_cast<size_t>(n);
    portENTER_CRITICAL(&s_lock);
    write_locked(buf, len);
    portEXIT_CRITICAL(&s_lock);
    return n;
}

void drain_task(void*) {
    char chunk[kChunkBytes];
    for (;;) {
        portENTER_CRITICAL(&s_lock);
        const size_t available = buffered_locked();
        const size_t take = (available < kChunkBytes) ? available : kChunkBytes;
        if (take != 0) {
            const size_t first = (kRingBytes - s_tail < take) ? (kRingBytes - s_tail) : take;
            memcpy(chunk, &s_ring[s_tail], first);
            if (take > first) {
                memcpy(chunk + first, &s_ring[0], take - first);
            }
            s_tail = (s_tail + take) % kRingBytes;
        }
        const uint32_t dropped = s_dropped;
        portEXIT_CRITICAL(&s_lock);

        if (take != 0) {
            // stdout keeps the newlib per-FILE lock, so chunks are atomic
            // against direct printf users (the screenshot dump) - no worse
            // interleaving than concurrent ESP_LOG callers produce today.
            fwrite(chunk, 1, take, stdout);
        }

        // Mark gaps inline so a spliced log is never mistaken for a complete
        // one. Goes through the ring like everything else.
        if (dropped != s_dropped_reported) {
            char note[64];
            const int n = snprintf(note,
                                   sizeof(note),
                                   "\n[log_ring] dropped %lu bytes total\n",
                                   static_cast<unsigned long>(dropped));
            s_dropped_reported = dropped;
            if (n > 0) {
                portENTER_CRITICAL(&s_lock);
                write_locked(note, static_cast<size_t>(n));
                portEXIT_CRITICAL(&s_lock);
            }
        }

        fflush(stdout);
        vTaskDelay(kDrainPeriod);
    }
}

}  // namespace

extern "C" esp_err_t wavex_log_ring_install(void) {
    // Start the drain first so no window exists where writes accumulate with
    // nothing scheduled to move them. Lowest runnable priority: log output
    // must never compete with real work.
    const BaseType_t ok =
        xTaskCreate(drain_task, "log_drain", 3072, nullptr, tskIDLE_PRIORITY + 1, nullptr);
    if (ok != pdPASS) {
        return ESP_FAIL;
    }
    esp_log_set_vprintf(ring_vprintf);
    return ESP_OK;
}

extern "C" uint32_t wavex_log_ring_dropped_bytes(void) {
    portENTER_CRITICAL(&s_lock);
    const uint32_t dropped = s_dropped;
    portEXIT_CRITICAL(&s_lock);
    return dropped;
}
