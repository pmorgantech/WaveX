#pragma once

#include <cstddef>
#include <cstdint>

namespace wavex_midi {
// One MIDI 1.0 streaming interface, alternate zero, first embedded cable.
// Descriptor bytes belong to the host stack; only this validated value escapes.
struct UsbMidiInterface {
    uint8_t number = 0, in = 0, out = 0;
    uint16_t in_size = 0, out_size = 0;
};

inline bool FindUsbMidiInterface(const uint8_t* bytes, size_t size, UsbMidiInterface& result) {
    if (!bytes || size < 9 || bytes[0] != 9 || bytes[1] != 2)
        return false;
    const size_t total = size_t(bytes[2]) | (size_t(bytes[3]) << 8);
    if (total < 9 || total > size)
        return false;
    UsbMidiInterface candidate{}, found{};
    bool midi = false, midi1 = false, have = false;
    auto finish = [&] {
        if (!have && midi && midi1 && candidate.in) {
            found = candidate;
            have = true;
        }
    };
    for (size_t pos = 9; pos < total;) {
        if (total - pos < 2)
            return false;
        const auto* d = bytes + pos;
        const size_t length = d[0];
        if (length < 2 || length > total - pos)
            return false;
        if (d[1] == 4) {
            if (length < 9)
                return false;
            finish();
            candidate = {};
            candidate.number = d[2];
            midi = d[3] == 0 && d[5] == 1 && d[6] == 3 && d[7] == 0;
            midi1 = false;
        } else if (midi && d[1] == 0x24 && length >= 3 && d[2] == 1) {
            if (length < 7)
                return false;
            midi1 = d[3] == 0 && d[4] == 1;  // bcdMSC 1.00, not UMP
        } else if (midi && d[1] == 5) {
            if (length < 7)
                return false;
            const uint16_t packet = uint16_t(d[4]) | uint16_t(uint16_t(d[5]) << 8);
            if ((d[3] & 3) != 2 || !(d[2] & 15) || (d[2] & 0x70) || packet < 4 || packet > 512 ||
                packet % 4)
                return false;
            if (d[2] & 0x80) {
                if (candidate.in)
                    return false;
                candidate.in = d[2];
                candidate.in_size = packet;
            } else {
                if (candidate.out)
                    return false;
                candidate.out = d[2];
                candidate.out_size = packet;
            }
        }
        pos += length;
    }
    finish();
    if (have)
        result = found;
    return have;
}

// USB-MIDI 1.0 Table 4-1. Reserved CINs and other cables are not MIDI bytes.
// Validate complete messages before replaying them through the stream parser.
inline uint8_t UsbMidiPacketSize(const uint8_t* p) {
    if (p[0] >> 4)
        return 0;
    constexpr uint8_t sizes[] = {0, 0, 2, 3, 3, 1, 2, 3, 3, 3, 3, 3, 2, 2, 3, 1};
    const auto cin = p[0] & 15;
    const auto size = sizes[cin];
    if (!size)
        return 0;
    if (cin >= 8 && cin <= 14) {
        if ((p[1] >> 4) != cin)
            return 0;
        for (unsigned i = 2; i <= size; ++i)
            if (p[i] & 0x80)
                return 0;
    } else if (cin == 2 || cin == 3) {
        if ((cin == 2 && p[1] != 0xf1 && p[1] != 0xf3) || (cin == 3 && p[1] != 0xf2))
            return 0;
        for (unsigned i = 2; i <= size; ++i)
            if (p[i] & 0x80)
                return 0;
    } else if (cin == 15) {
        // The single-byte form also permits running-status/SysEx bytes.
        return 1;
    } else if (cin == 5) {
        if (p[1] != 0xf6 && p[1] != 0xf7)
            return 0;
    } else {
        const bool end = cin == 6 || cin == 7;
        if (end && p[size] != 0xf7)
            return 0;
        for (unsigned i = 1; i <= size - unsigned(end); ++i)
            if ((p[i] & 0x80) && !(i == 1 && p[i] == 0xf0))
                return 0;
    }
    return size;
}
}  // namespace wavex_midi
