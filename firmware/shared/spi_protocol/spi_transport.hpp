#pragma once

#include "../uart_protocol/uart_protocol.h"
#include "protocol.h"

#include "sequence_tracker.hpp"
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace WaveX {
namespace Protocol {
namespace Spi {

// Reuse the live length-bearing codec on both links. The older padded packet
// format cannot represent exact payload lengths required by current handlers.
// Round the maximum framed payload to full 128-byte P4/M7 cache lines.
constexpr size_t kFrameBytes =
    ((UartProtocol::UART_MAX_PAYLOAD + UartProtocol::UART_FRAME_OVERHEAD + 127) / 128) * 128;
constexpr size_t kMaxPayload = UartProtocol::UART_MAX_PAYLOAD;
constexpr uint32_t kTransferTimeoutMs = 100;
constexpr uint32_t kPollIntervalMs = 5;

// Foreground-owned on Daisy; protected by the link mutex on ESP32. The head is
// immutable until Finish(), including when Begin() reserved an empty frame.
class TxQueue {
   public:
    bool Push(const uint8_t* packet, size_t bytes) {
        if (!packet || bytes > kFrameBytes || Full() ||
            !UartProtocol::ValidateUartFrame(packet, bytes))
            return false;
        std::memcpy(packets_[tail_], packet, bytes);
        sizes_[tail_] = bytes;
        tail_ = (tail_ + 1) % kCapacity;
        ++count_;
        return true;
    }

    bool Begin(uint8_t* frame) {
        if (owned_ || !frame)
            return false;
        owned_ = true;
        has_packet_ = count_ != 0;
        std::memset(frame, 0, kFrameBytes);
        if (has_packet_)
            std::memcpy(frame, packets_[head_], sizes_[head_]);
        return true;
    }

    // Only call after the driver returned ownership and RX was retained, or
    // after a failed launch/abort has proved that DMA cannot touch the buffers.
    bool Finish(bool full_transfer) {
        if (!owned_)
            return false;
        const bool sent = full_transfer && has_packet_;
        if (sent) {
            head_ = (head_ + 1) % kCapacity;
            --count_;
        }
        owned_ = false;
        has_packet_ = false;
        return sent;
    }

    void Reset() {
        // Only the lifecycle owner may reset an unowned queue.
        if (owned_)
            return;
        head_ = tail_ = count_ = 0;
        has_packet_ = false;
    }

    bool Owned() const { return owned_; }
    size_t Count() const { return count_; }
    bool Full() const { return count_ == kCapacity; }

   private:
    static constexpr size_t kCapacity = 8;
    uint8_t packets_[kCapacity][kFrameBytes] = {};
    size_t sizes_[kCapacity] = {};
    size_t head_ = 0;
    size_t tail_ = 0;
    size_t count_ = 0;
    bool owned_ = false;
    bool has_packet_ = false;
};

inline uint16_t NextSequence(uint16_t current) {
    return current == UINT16_MAX ? 1 : static_cast<uint16_t>(current + 1);
}

// READY goes high only after the slave loads its DMA descriptor, and low in
// its completion ISR. Consume each assertion once. A falling-edge generation
// preserves short low pulses even when the foreground is busy with audio/SD.
class ReadyGate {
   public:
    bool Observe(bool high, uint32_t falling_edges) {
        if (!high || falling_edges != consumed_edges_)
            fresh_ = true;
        return high && fresh_;
    }

    void Consume(uint32_t falling_edges) {
        consumed_edges_ = falling_edges;
        fresh_ = false;
    }

   private:
    uint32_t consumed_edges_ = 0;
    bool fresh_ = true;  // A slave already READY at startup is valid.
};

// Only the DMA completion ISR writes a terminal state. No ISR may prepare a
// frame, route a packet, or launch SPI. Release/acquire publishes the RX bytes.
class MasterTransfer {
   public:
    enum class State : uint32_t { Idle, Running, Complete, Failed, Stopping };

    bool Reserve(uint32_t now_ms) {
        if (Get() != State::Idle)
            return false;
        started_ms_ = now_ms;
        state_.store(State::Running, std::memory_order_release);
        return true;
    }

    void CompleteFromIsr(bool success) {
        auto expected = State::Running;
        state_.compare_exchange_strong(expected,
                                       success ? State::Complete : State::Failed,
                                       std::memory_order_release,
                                       std::memory_order_relaxed);
    }

    bool Expired(uint32_t now_ms) const {
        return Get() == State::Running &&
               static_cast<uint32_t>(now_ms - started_ms_) >= kTransferTimeoutMs;
    }

    // Caller first masks the SPI/DMA IRQs; audio interrupts stay enabled.
    void Stop() { state_.store(State::Stopping, std::memory_order_release); }
    void Release() { state_.store(State::Idle, std::memory_order_release); }
    State Get() const { return state_.load(std::memory_order_acquire); }

   private:
    static_assert(std::atomic<State>::is_always_lock_free, "ISR state must be lock-free");
    std::atomic<State> state_{State::Idle};
    uint32_t started_ms_ = 0;
};

enum class RxResult { Empty, Invalid, Duplicate, Packet };

// Validate before inspecting the sequence or routing. Actual physical length
// must equal the agreed frame size; a partial DMA result is never a packet.
inline RxResult InspectFrame(const uint8_t* frame,
                             size_t transferred_bytes,
                             SequenceTracker& sequence,
                             size_t& packet_bytes) {
    packet_bytes = 0;
    if (!frame || transferred_bytes != kFrameBytes)
        return RxResult::Invalid;
    bool empty = true;
    for (size_t i = 0; i < kFrameBytes; ++i)
        empty = empty && frame[i] == 0;
    if (empty)
        return RxResult::Empty;
    const size_t size = UartProtocol::GetFrameLength(frame, transferred_bytes);
    if (size == 0 || size > transferred_bytes || !UartProtocol::ValidateUartFrame(frame, size))
        return RxResult::Invalid;
    const uint16_t seq = static_cast<uint16_t>(frame[5] | (uint16_t(frame[6]) << 8));
    const auto result = sequence.Evaluate(seq);
    if (result == SequenceTracker::Result::Duplicate ||
        result == SequenceTracker::Result::OutOfOrder)
        return RxResult::Duplicate;
    packet_bytes = size;
    return RxResult::Packet;
}

}  // namespace Spi
}  // namespace Protocol
}  // namespace WaveX
