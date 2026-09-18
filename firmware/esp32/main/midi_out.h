#pragma once
#include "midi/clock_output.hpp"
namespace wavex_midi {
enum class Port : uint8_t { Din, Usb };
constexpr uint8_t kDin = 1, kUsb = 2, kBoth = kDin | kUsb;
// Task-context producer API. Returns the mask actually queued; never performs I/O.
uint8_t SendClock(const WaveX::Protocol::SeqClockOutMessage& message, uint8_t ports = kBoth);
WaveX::Midi::OutputStats Status(Port port);
// Only the corresponding port task changes readiness, consumes or completes.
void Ready(Port port, bool ready);
bool Take(Port port, WaveX::Midi::ClockPacket& packet);
void Complete(Port port, bool success);
}  // namespace wavex_midi
