// WaveX current-track: which Track every page acts on
#pragma once

#include <cstdint>

namespace wavex_ui {

/// The Track that Play sends notes on, Sample Manager's Select binds to,
/// Voice edits, and the Sample Browser loads an SFZ Instrument into
/// (track-and-patch-model.md §1). Before this existed each of those pages
/// kept a private copy, so "which Track?" had four different answers at once
/// and an Instrument always landed on Track 1 regardless of what was selected.
///
/// Stored 0-based, because that is what MSG_NOTE_ON's channel and
/// kNumInstrumentSlots use on the wire. Every user-facing string is 1-based:
/// format it through trackDisplayNumber() rather than adding 1 by hand, so
/// the convention lives in one place.
uint8_t getCurrentTrack();
void setCurrentTrack(uint8_t track);

/// 1-based number to print for a 0-based Track index.
inline unsigned trackDisplayNumber(uint8_t track) {
    return static_cast<unsigned>(track) + 1u;
}

}  // namespace wavex_ui
