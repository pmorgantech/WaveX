#pragma once

// Reboot-aware sequence-number duplicate/out-of-order tracker (roadmap
// Phase 1 item 7: "sequence-number resync"). Single shared implementation
// for logic that used to be duplicated byte-for-byte between
// daisy_spi_link.cpp and esp_spi_link.cpp's is_duplicate_packet().
//
// Real bug this fixes: the old duplicated logic rejected any incoming
// sequence number more than kReorderTolerance below what it currently
// expected as "out-of-order," and dropped it - forever, since nothing ever
// reset the expected-sequence state. Sequence counters reset to 1 on link
// init on both MCUs (0 is reserved - see protocol.h/uart_protocol.h). So if
// a peer rebooted mid-session, every packet it ever sent again would be
// compared against the receiver's still-high expectation and permanently
// classified as out-of-order: a wedge that only a receiver-side restart
// could clear. Evaluate() detects that specific pattern - a low,
// fresh-looking sequence number arriving far below an already-advanced
// expectation - and treats it as a legitimate resync instead of corruption
// or ordinary network reordering.
//
// HAL-free: plain integer arithmetic, host-testable.

#include <cstdint>

namespace WaveX {
namespace Protocol {

class SequenceTracker {
   public:
    enum class Result : uint8_t {
        Accept,        // normal in-order (or within-tolerance-reordered) packet
        Duplicate,     // exact repeat of the last accepted sequence number
        OutOfOrder,    // too far behind expectation to be explained by reordering or a reboot
        ResyncAccept,  // classified as a peer reboot; state resynced to follow the new stream
    };

    // Reordering tolerance: an incoming seq up to this far below
    // `expected_seq_` is still accepted as ordinary network/DMA reordering
    // (matches the original is_duplicate_packet's behavior).
    static constexpr uint16_t kReorderTolerance = 10;
    // A low sequence number (<= this) looks like a freshly-reset sender.
    static constexpr uint16_t kFreshStartMax = 5;
    // Only treat a low/fresh sequence number as a reboot if the tracker had
    // already advanced well past ordinary startup - avoids misclassifying
    // early-session reordering (e.g. expected_seq_=15, seq=3) as a reboot.
    static constexpr uint16_t kResyncMinPriorProgress = 100;

    // seq == 0 is always rejected (reserved/invalid per the wire protocol).
    Result Evaluate(uint16_t seq) {
        if (seq == 0)
            return Result::OutOfOrder;

        if (seq == last_received_seq_) {
            ++duplicate_count_;
            return Result::Duplicate;
        }

        const uint16_t expected_min =
            (expected_seq_ > kReorderTolerance) ? (expected_seq_ - kReorderTolerance) : 1;
        if (seq < expected_min) {
            const bool looks_like_reboot =
                (seq <= kFreshStartMax) && (expected_seq_ > kResyncMinPriorProgress);
            if (looks_like_reboot) {
                ++resync_count_;
                last_received_seq_ = seq;
                expected_seq_ = seq + 1;
                return Result::ResyncAccept;
            }
            ++out_of_order_count_;
            return Result::OutOfOrder;
        }

        last_received_seq_ = seq;
        expected_seq_ = seq + 1;
        return Result::Accept;
    }

    uint32_t DuplicateCount() const { return duplicate_count_; }
    uint32_t OutOfOrderCount() const { return out_of_order_count_; }
    uint32_t ResyncCount() const { return resync_count_; }
    uint16_t ExpectedSeq() const { return expected_seq_; }

   private:
    uint16_t last_received_seq_ = 0;
    uint16_t expected_seq_ = 1;  // 0 is reserved; counting starts at 1
    uint32_t duplicate_count_ = 0;
    uint32_t out_of_order_count_ = 0;
    uint32_t resync_count_ = 0;
};

}  // namespace Protocol
}  // namespace WaveX
