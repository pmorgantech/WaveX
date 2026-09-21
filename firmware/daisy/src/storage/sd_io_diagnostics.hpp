#pragma once

#include <cstdint>

namespace WaveX::Storage::SdIo {

struct Registers {
    uint32_t status = 0, remaining = 0, dma = 0, buffer = 0;
    uint32_t clock = 0, command = 0, argument = 0, response = 0;
    uint32_t error = 0, state = 0, context = 0;
};

struct Failure {
    bool valid = false;
    bool write = false;
    bool irq = false;
    uint8_t drive = 0;
    uint32_t sequence = 0, sector = 0, count = 0, buffer = 0;
    uint32_t elapsed_ms = 0, result = 0;
    Registers registers{};
};

// One foreground disk operation at a time. The device adapter masks interrupts
// while publishing/completing operations; only ObserveIrq runs in the SD ISR.
// The first completed failure remains immutable until an explicit Reset.
class FirstFailure {
   public:
    void Begin(bool write, uint8_t drive, uint32_t sector, uint32_t count, uint32_t buffer) {
        current_ = {};
        current_.write = write;
        current_.drive = drive;
        current_.sector = sector;
        current_.count = count;
        current_.buffer = buffer;
        current_.sequence = ++sequence_;
        active_ = true;
    }
    void ObserveIrq(const Registers& registers) {
        if (active_ && !first_.valid && !current_.irq) {
            current_.irq = true;
            current_.registers = registers;
        }
    }
    void Finish(uint32_t result, uint32_t elapsed, const Registers& registers) {
        if (active_ && !first_.valid && (result != 0 || current_.irq)) {
            current_.valid = true;
            current_.result = result;
            current_.elapsed_ms = elapsed;
            if (!current_.irq)
                current_.registers = registers;
            first_ = current_;
        }
        active_ = false;
    }
    bool HasFailure() const { return first_.valid; }
    Failure Snapshot() const { return first_; }
    void Reset() { first_ = {}; }

   private:
    Failure current_{}, first_{};
    uint32_t sequence_ = 0;
    bool active_ = false;
};

Failure Snapshot();
void Reset();
// Foreground-only, emits each latched failure once through the existing ring.
void Report();

}  // namespace WaveX::Storage::SdIo
