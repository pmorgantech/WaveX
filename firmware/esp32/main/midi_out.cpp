#include "midi_out.h"

#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
namespace wavex_midi {
namespace {
portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
WaveX::Midi::ClockOutputQueue queues[2];
uint32_t nowMs() {
    return static_cast<uint32_t>(esp_timer_get_time() / 1000);
}
size_t index(Port port) {
    return static_cast<size_t>(port);
}
}  // namespace
uint8_t SendClock(const WaveX::Protocol::SeqClockOutMessage& message, uint8_t ports) {
    WaveX::Midi::ClockPacket packet;
    if (!ports || (ports & ~kBoth) || !WaveX::Midi::EncodeClock(message, packet))
        return 0;
    const auto now = nowMs();
    uint8_t accepted = 0;
    portENTER_CRITICAL(&mux);
    for (size_t i = 0; i < 2; ++i)
        if ((ports & (1u << i)) && queues[i].Push(packet, now))
            accepted |= static_cast<uint8_t>(1u << i);
    portEXIT_CRITICAL(&mux);
    return accepted;
}
void Ready(Port port, bool ready) {
    portENTER_CRITICAL(&mux);
    queues[index(port)].Ready(ready);
    portEXIT_CRITICAL(&mux);
}
bool Take(Port port, WaveX::Midi::ClockPacket& packet) {
    const auto now = nowMs();
    portENTER_CRITICAL(&mux);
    const bool result = queues[index(port)].Pop(now, packet);
    portEXIT_CRITICAL(&mux);
    return result;
}
void Complete(Port port, bool success) {
    portENTER_CRITICAL(&mux);
    queues[index(port)].Complete(success);
    portEXIT_CRITICAL(&mux);
}
WaveX::Midi::OutputStats Status(Port port) {
    portENTER_CRITICAL(&mux);
    const auto result = queues[index(port)].Stats();
    portEXIT_CRITICAL(&mux);
    return result;
}
}  // namespace wavex_midi
