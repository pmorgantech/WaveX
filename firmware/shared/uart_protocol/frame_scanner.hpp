#pragma once

// Shared UART RX frame scanner (review P3.14 / §6.2). Both links carried
// byte-for-byte-similar scanning loops that had already diverged once with
// real consequences: the H2 no-start-byte drain guard was fixed on the
// ESP32 side only, leaving the Daisy able to wedge permanently on garbage.
// This class is the single implementation of the scan/consume policy;
// per-platform code keeps only its transport glue (DMA circular buffer vs
// driver ring) and its dispatch.
//
// HAL-free and allocation-free: storage is caller-provided, synchronization
// is the caller's job (both links run Append/Scan from a single context),
// and the framing primitives are the existing FindFrameStart /
// GetFrameLength / ValidateUartFrame from uart_protocol.h - so the policy
// is host-testable (tests/protocol/frame_scanner_test.cpp).

#include "uart_protocol.h"

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace WaveX {
namespace UartProtocol {

struct ScanStats {
    uint32_t frames = 0;         // valid frames dispatched
    uint32_t crc_errors = 0;     // framed but CRC/end-byte invalid (resynced +1 byte)
    uint32_t sync_errors = 0;    // start byte with an impossible length field (resynced +1 byte)
    uint32_t dropped_bytes = 0;  // overflow drops (oldest-first) via Append
};

class FrameScanner {
   public:
    FrameScanner(uint8_t* storage, size_t capacity) : buf_(storage), cap_(capacity) {}

    // Appends bytes to the scan buffer. On overflow the OLDEST buffered
    // bytes are dropped (frame boundaries recover via the start-byte scan);
    // if `len` alone exceeds the capacity only its newest bytes are kept.
    // Returns the number of bytes dropped (0 in the normal case).
    size_t Append(const uint8_t* data, size_t len, ScanStats& stats) {
        if (!data || len == 0) {
            return 0;
        }
        size_t dropped = 0;
        if (len > cap_) {
            dropped += len - cap_;
            data += len - cap_;
            len = cap_;
        }
        if (len_ + len > cap_) {
            const size_t overflow = (len_ + len) - cap_;
            Consume(overflow);
            dropped += overflow;
        }
        std::memcpy(buf_ + len_, data, len);
        len_ += len;
        // dropped_bytes is a uint32_t diagnostic counter; a single Append
        // cannot drop more than the buffer capacity, so this cannot lose
        // information for any capacity this firmware uses.
        stats.dropped_bytes += static_cast<uint32_t>(dropped);
        return dropped;
    }

    // Extracts every complete, valid frame currently buffered, calling
    // on_frame(frame, frame_len) for each. Policy (identical to what both
    // links converged on, plus the bogus-length resync):
    //  - CRC/end-byte failure on a framed candidate: count, resync +1 byte.
    //  - Start byte with an impossible length field: count, resync +1 byte
    //    (previously this was treated as "incomplete" and stalled until the
    //    buffer overflowed).
    //  - No start byte anywhere: discard the scanned window except the
    //    trailing UART_FRAME_OVERHEAD-1 bytes, which FindFrameStart cannot
    //    scan and may yet begin a frame (the H2 wedge guard).
    //  - Incomplete frame at the tail: keep it buffered for the next Append.
    template <typename Fn>
    void Scan(Fn&& on_frame, ScanStats& stats) {
        size_t offset = 0;
        while (len_ - offset >= UART_FRAME_OVERHEAD) {
            const int start = FindFrameStart(buf_ + offset, len_ - offset);
            if (start < 0) {
                const size_t keep = UART_FRAME_OVERHEAD - 1;
                if (len_ > keep) {
                    Consume(len_ - keep);
                }
                return;
            }
            offset += static_cast<size_t>(start);

            const size_t available = len_ - offset;
            const size_t frame_len = GetFrameLength(buf_ + offset, available);
            if (frame_len == 0) {
                // Start byte but a length field no frame can have: not a
                // frame boundary. Skip the start byte and rescan.
                stats.sync_errors++;
                offset += 1;
                continue;
            }
            if (frame_len > available) {
                break;  // genuinely incomplete; wait for more bytes
            }
            if (!ValidateUartFrame(buf_ + offset, frame_len)) {
                stats.crc_errors++;
                offset += 1;
                continue;
            }
            stats.frames++;
            on_frame(buf_ + offset, frame_len);
            offset += frame_len;
        }
        Consume(offset);
    }

    size_t Buffered() const { return len_; }
    size_t Capacity() const { return cap_; }
    void Clear() { len_ = 0; }

   private:
    void Consume(size_t n) {
        if (n == 0) {
            return;
        }
        if (n >= len_) {
            len_ = 0;
            return;
        }
        std::memmove(buf_, buf_ + n, len_ - n);
        len_ -= n;
    }

    uint8_t* buf_;
    size_t cap_;
    size_t len_ = 0;
};

}  // namespace UartProtocol
}  // namespace WaveX
