#pragma once
#include "spi_protocol/protocol.h"

#include <array>
#include <cstddef>
#include <cstdint>

namespace WaveX::Midi {
// The engine owns clock generation. Ports receive complete immutable messages.
struct ClockPacket {
    std::array<uint8_t, 3> bytes{};
    uint8_t size = 0;
    std::array<uint8_t, 4> Usb() const {
        return {static_cast<uint8_t>(size == 3 ? 0x03 : 0x0f), bytes[0], bytes[1], bytes[2]};
    }
};
inline bool EncodeClock(const Protocol::SeqClockOutMessage& message, ClockPacket& packet) {
    if (message.reserved || message.reserved2 || message.event > Protocol::MIDI_CLK_SPP ||
        message.spp_beats16 > 0x3fff ||
        (message.event != Protocol::MIDI_CLK_SPP && message.spp_beats16 != 0))
        return false;
    constexpr uint8_t status[]{0xf8, 0xfa, 0xfb, 0xfc, 0xf2};
    packet = {};
    packet.bytes[0] = status[message.event];
    packet.size = message.event == Protocol::MIDI_CLK_SPP ? 3 : 1;
    if (packet.size == 3) {
        packet.bytes[1] = static_cast<uint8_t>(message.spp_beats16 & 0x7f);
        packet.bytes[2] = static_cast<uint8_t>(message.spp_beats16 >> 7);
    }
    return true;
}
struct OutputStats {
    uint32_t accepted = 0, sent = 0, dropped = 0, expired = 0, failed = 0;
    uint8_t pending = 0;
    bool ready = false;
};
// Caller serializes access. No allocation, retries or timing generation here.
class ClockOutputQueue {
   public:
    static constexpr size_t kCapacity = 32;
    static constexpr uint32_t kMaxAgeMs = 50;
    void Ready(bool ready) {
        if (ready != stats_.ready) {
            Clear();
            stats_.ready = ready;
        }
    }
    bool Push(const ClockPacket& packet, uint32_t now) {
        if (!stats_.ready) {
            ++stats_.dropped;
            return false;
        }
        // A new transport boundary must not sit behind old clock backlog.
        if (packet.bytes[0] == 0xfa || packet.bytes[0] == 0xfc)
            Clear();
        if (stats_.pending == kCapacity) {
            ++stats_.dropped;
            return false;
        }
        entries_[(head_ + stats_.pending) % kCapacity] = {packet, now};
        ++stats_.pending;
        ++stats_.accepted;
        return true;
    }
    bool Pop(uint32_t now, ClockPacket& packet) {
        while (stats_.pending) {
            const auto entry = entries_[head_];
            head_ = (head_ + 1) % kCapacity;
            --stats_.pending;
            // A late Stop is still useful; a late tick/start/position is not.
            if (entry.packet.bytes[0] != 0xfc && now - entry.at > kMaxAgeMs) {
                ++stats_.expired;
                continue;
            }
            packet = entry.packet;
            return true;
        }
        return false;
    }
    void Complete(bool success) {
        if (success)
            ++stats_.sent;
        else
            ++stats_.failed;
    }
    OutputStats Stats() const { return stats_; }

   private:
    void Clear() {
        stats_.dropped += stats_.pending;
        stats_.pending = 0;
        head_ = 0;
    }
    struct Entry {
        ClockPacket packet;
        uint32_t at = 0;
    };
    std::array<Entry, kCapacity> entries_{};
    size_t head_ = 0;
    OutputStats stats_;
};
}  // namespace WaveX::Midi
