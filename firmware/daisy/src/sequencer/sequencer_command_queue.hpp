#pragma once

// Fixed SPSC hand-off from Daisy's main-loop message dispatcher to the audio
// callback. SequencerTransport is callback-owned; this queue is the only path
// through which wire commands reach it, so no callback reads mutable foreground
// state. No allocation, locks, or HAL calls are involved.

#include "spi_protocol/protocol.h"

#include <cstdint>
#include <type_traits>

namespace WaveX {
namespace Sequencer {

enum class SequencerCommandType : uint8_t {
    Transport,
    PatternOp,
    MidiClock,
    MidiCc,
};

struct SequencerCommand {
    SequencerCommandType type = SequencerCommandType::Transport;
    Protocol::SeqTransportMessage transport{};
    Protocol::SeqPatternOpMessage pattern_op{};
    Protocol::MidiClockEventMessage midi_clock{};
    Protocol::MidiCcMessage midi_cc{};
};

template <uint32_t Capacity>
class SequencerCommandQueue {
   public:
    static_assert(Capacity > 0, "Sequencer command queue capacity must be non-zero");
    static_assert(std::is_trivially_copyable<SequencerCommand>::value,
                  "Sequencer commands cross the IRQ boundary by value");

    void Init() {
        __atomic_store_n(&write_, 0u, __ATOMIC_RELAXED);
        __atomic_store_n(&read_, 0u, __ATOMIC_RELAXED);
    }

    // Main-loop producer. False means the bounded queue is full; callers log
    // that loss in foreground context rather than ever blocking audio.
    bool Push(const SequencerCommand& command) {
        const uint32_t write = __atomic_load_n(&write_, __ATOMIC_RELAXED);
        const uint32_t read = __atomic_load_n(&read_, __ATOMIC_ACQUIRE);
        if (write - read >= Capacity)
            return false;
        commands_[write % Capacity] = command;
        __atomic_store_n(&write_, write + 1u, __ATOMIC_RELEASE);
        return true;
    }

    // Audio-callback consumer.
    bool Pop(SequencerCommand& command) {
        const uint32_t read = __atomic_load_n(&read_, __ATOMIC_RELAXED);
        const uint32_t write = __atomic_load_n(&write_, __ATOMIC_ACQUIRE);
        if (read == write)
            return false;
        command = commands_[read % Capacity];
        __atomic_store_n(&read_, read + 1u, __ATOMIC_RELEASE);
        return true;
    }

   private:
    SequencerCommand commands_[Capacity] = {};
    uint32_t write_ = 0;
    uint32_t read_ = 0;
};

}  // namespace Sequencer
}  // namespace WaveX
