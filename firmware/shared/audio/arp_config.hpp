#pragma once
#include <cstdint>
namespace WaveX::Arp {
// Instrument-owned, persisted verbatim in the optional eight-byte ARP chunk.
// Division is an index; tick values use the sequencer's 96 PPQN clock.
inline constexpr uint16_t kTicks[] = {12, 16, 18, 24, 32, 36, 48, 64, 72, 96, 144, 192};
inline constexpr const char* kDivisionNames[] = {
    "1/32", "1/16T", "1/32D", "1/16", "1/8T", "1/16D", "1/8", "1/4T", "1/8D", "1/4", "1/4D", "1/2"};
inline constexpr const char* kModeNames[] = {
    "Up", "Down", "Up/down repeat", "Up/down", "As played", "Random"};
struct Config {
    uint8_t enabled = 0, mode = 0, octaves = 1, division = 3;
    uint8_t gate_pct = 75, latch = 0, vel_mode = 0, vel_fixed = 100;
} __attribute__((packed));
static_assert(sizeof(Config) == 8);
inline bool Valid(const Config& c) {
    return c.enabled <= 1 && c.mode < 6 && c.octaves >= 1 && c.octaves <= 4 && c.division < 12 &&
           c.gate_pct >= 1 && c.gate_pct <= 100 && c.latch <= 1 && c.vel_mode < 3 &&
           c.vel_fixed >= 1 && c.vel_fixed <= 127;
}
inline bool Equal(const Config& a, const Config& b) {
    return a.enabled == b.enabled && a.mode == b.mode && a.octaves == b.octaves &&
           a.division == b.division && a.gate_pct == b.gate_pct && a.latch == b.latch &&
           a.vel_mode == b.vel_mode && a.vel_fixed == b.vel_fixed;
}
}  // namespace WaveX::Arp
