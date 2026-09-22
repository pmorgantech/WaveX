#pragma once

#include "spi_protocol/protocol.h"

#include <array>
#include <cstddef>
#include <cstdint>

namespace WaveX::Comm {
// Caller serializes Send/Service. A slot belongs to one admitted note address,
// not a page. Admission reserves room for every matching release before sending
// note-on. Counts preserve repeated presses; rejected note-ons are never queued.
class NoteDelivery {
   public:
    static constexpr size_t kCapacity = 128;
    template <typename Sender>
    bool Send(uint8_t type, const Protocol::NoteMessage& message, Sender&& send) {
        if (message.note > 127 || message.velocity > 127)
            return false;
        Slot* slot = nullptr;
        Slot* free = nullptr;
        for (auto& candidate: slots_) {
            if (candidate.held == 0 && candidate.releases == 0) {
                if (!free)
                    free = &candidate;
            } else if (candidate.note == message.note && candidate.channel == message.channel) {
                slot = &candidate;
                break;
            }
        }
        const bool on = type == Protocol::MSG_NOTE_ON && message.velocity != 0;
        if (on) {
            if (!slot)
                slot = free;
            if (!slot || slot->releases || slot->held == UINT16_MAX)
                return false;
            if (!send(Protocol::MSG_NOTE_ON, message))
                return false;
            slot->note = message.note;
            slot->channel = message.channel;
            ++slot->held;
            return true;
        }
        if (!slot || !slot->held)
            return true;  // No admitted press to release (or already pending).
        --slot->held;
        ++slot->releases;  // held + releases never exceeds the admitted count.
        TryRelease(*slot, send);
        return true;  // Client owns retry even if the link is currently full/down.
    }

    template <typename Sender>
    void Service(Sender&& send) {
        size_t sent = 0;
        for (size_t visited = 0; visited < kCapacity && sent < 8; ++visited) {
            Slot& slot = slots_[cursor_];
            cursor_ = (cursor_ + 1) % kCapacity;
            if (!slot.releases)
                continue;
            if (!TryRelease(slot, send))
                return;  // Stop at backpressure.
            ++sent;
        }
    }

   private:
    struct Slot {
        uint16_t held = 0;
        uint16_t releases = 0;
        uint8_t note = 0;
        uint8_t channel = 0;
    };
    template <typename Sender>
    bool TryRelease(Slot& slot, Sender& send) {
        Protocol::NoteMessage off;
        off.note = slot.note;
        off.channel = slot.channel;
        off.velocity = 0;
        off.reserved = 0;
        if (!send(Protocol::MSG_NOTE_OFF, off))
            return false;
        --slot.releases;
        return true;
    }
    std::array<Slot, kCapacity> slots_{};
    size_t cursor_ = 0;
};
}  // namespace WaveX::Comm
