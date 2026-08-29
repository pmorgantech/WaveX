#include "log_ring.h"

#include "daisy_seed.h"

#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace WaveX {
namespace Log {

namespace {

// 8 KiB absorbs the burstiest case seen in practice (a wall of underrun and
// stream-telemetry lines) without being large enough to matter against the
// 512 KiB AXI SRAM budget.
constexpr size_t kRingBytes = 8192;
// USB FS bulk packet size. One packet per drain keeps the per-pass cost
// fixed and small.
constexpr size_t kPacketBytes = 64;

char s_ring[kRingBytes];
volatile size_t s_head = 0;  // write position
volatile size_t s_tail = 0;  // read position
uint32_t s_dropped = 0;

daisy::DaisySeed* s_hw = nullptr;

// Ping-pong staging. CDC_Transmit_FS does not copy - it hands the pointer to
// the USB stack - so a buffer must stay untouched until its transfer
// finishes. Alternating means a buffer is only refilled after a LATER
// transmit has succeeded, and a transmit only succeeds once the previous one
// has completed (TxState clear). Refilling a single buffer would risk
// overwriting bytes still in flight.
char s_stage[2][kPacketBytes];
uint8_t s_stage_idx = 0;
size_t s_staged_len = 0;  // non-zero: a packet is staged and not yet accepted

size_t Buffered() {
    return (s_head - s_tail) % kRingBytes;
}

}  // namespace

void Init(daisy::DaisySeed* hw) {
    s_hw = hw;
}

void Write(const char* data, size_t len) {
    if (data == nullptr || len == 0) {
        return;
    }
    // A single write longer than the ring keeps only its tail; the newest
    // bytes are the ones worth having.
    if (len >= kRingBytes) {
        const size_t excess = len - (kRingBytes - 1);
        data += excess;
        len -= excess;
        s_dropped += excess;
    }

    // Make room by discarding oldest bytes rather than refusing the write.
    const size_t free_bytes = (kRingBytes - 1) - Buffered();
    if (len > free_bytes) {
        const size_t discard = len - free_bytes;
        s_tail = (s_tail + discard) % kRingBytes;
        s_dropped += discard;
    }

    const size_t head = s_head;
    const size_t first = (kRingBytes - head < len) ? (kRingBytes - head) : len;
    std::memcpy(&s_ring[head], data, first);
    if (len > first) {
        std::memcpy(&s_ring[0], data + first, len - first);
    }
    s_head = (head + len) % kRingBytes;
}

void Printf(const char* fmt, ...) {
    char buf[256];
    va_list args;
    va_start(args, fmt);
    const int n = vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    if (n <= 0) {
        return;
    }
    const size_t len =
        (static_cast<size_t>(n) >= sizeof(buf)) ? sizeof(buf) - 1 : static_cast<size_t>(n);
    Write(buf, len);
}

void PrintLine(const char* fmt, ...) {
    char buf[256];
    va_list args;
    va_start(args, fmt);
    const int n = vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    if (n <= 0) {
        return;
    }
    const size_t len =
        (static_cast<size_t>(n) >= sizeof(buf)) ? sizeof(buf) - 1 : static_cast<size_t>(n);
    Write(buf, len);
    Write("\r\n", 2);
}

void Drain() {
    if (s_hw == nullptr) {
        return;  // Pre-Init: keep accumulating, transmit once hardware exists.
    }

    // Stage a packet only when nothing is awaiting acceptance, so a busy
    // endpoint never causes the staged bytes to change underneath it.
    if (s_staged_len == 0) {
        const size_t available = Buffered();
        if (available == 0) {
            return;
        }
        const size_t take = (available < kPacketBytes) ? available : kPacketBytes;
        const size_t tail = s_tail;
        const size_t first = (kRingBytes - tail < take) ? (kRingBytes - tail) : take;
        std::memcpy(s_stage[s_stage_idx], &s_ring[tail], first);
        if (take > first) {
            std::memcpy(s_stage[s_stage_idx] + first, &s_ring[0], take - first);
        }
        s_staged_len = take;
    }

    // One non-blocking attempt. USBD_BUSY comes back as ERR; the packet stays
    // staged and the loop moves on instead of spinning.
    const auto result = s_hw->usb_handle.TransmitInternal(
        reinterpret_cast<uint8_t*>(s_stage[s_stage_idx]), s_staged_len);
    if (result == daisy::UsbHandle::Result::OK) {
        s_tail = (s_tail + s_staged_len) % kRingBytes;
        s_staged_len = 0;
        s_stage_idx ^= 1u;  // next packet uses the other buffer
    }
}

uint32_t DroppedBytes() {
    return s_dropped;
}

}  // namespace Log
}  // namespace WaveX
