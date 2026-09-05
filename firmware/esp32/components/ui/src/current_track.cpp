#include "ui/current_track.h"

#include "spi_protocol/protocol.h"

namespace wavex_ui {

namespace {
// Matches instrument.hpp's kNumTracks and MSG_NOTE_ON's channel
// masking on the backend; a value outside it would address a Track the Daisy
// would silently drop notes for.
constexpr uint8_t kTrackCount = WAVEX_MIX_TRACKS;

// Written and read only from the LVGL/UI task: every caller is a page
// handler, softkey callback or lv_timer callback, all of which run in LVGL
// context with its lock held. The UART RX task fills the binding cache in
// inter_mcu.cpp but never touches this, so no critical section is needed
// here (esp32p4_coding_guide.md §14, "shared dual-core state"). Anything
// reading this from another task must add synchronisation.
uint8_t s_current_track = 0;
}  // namespace

uint8_t getCurrentTrack() {
    return s_current_track;
}

void setCurrentTrack(uint8_t track) {
    if (track < kTrackCount) {
        s_current_track = track;
    }
}

}  // namespace wavex_ui
