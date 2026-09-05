// WaveX panel keys: the logical keys of the physical panel
#pragma once

#include <cstdint>

namespace wavex_ui {

/**
 * @brief Every key on the panel, by meaning rather than by matrix position.
 *
 * The keypad task translates a TCA8418 keycode into one of these through the
 * WAVEX_KEYCODE_* map in hardware_config.h, and it is what an InputEvent's
 * source_id carries for KeyPress/KeyRelease (and the older
 * ButtonPress/ButtonRelease) events. Which key means what is decided in one
 * place, InputDispatcher::processAll(): Shift, Back, the softkeys, the
 * root-menu jumps and Track -/+ are global and never reach a page; the
 * transport and pad keys are forwarded until Phase 2 gives them sequencer
 * semantics (panel-controls.md §4.3).
 *
 * The first four values are the ids the four bench buttons have always
 * posted, so the BUTTON_* aliases in ui_softkey.h are the same numbers.
 * HAL-free on purpose: the name table is host-tested.
 */
enum class PanelKey : uint8_t {
    None = 0,
    NavAPush,  ///< Nav encoder A push: Select (page-handled)
    Back,
    NavBPush,  ///< Nav encoder B push (page-handled)
    Shift,
    Soft1,  ///< Under the screen's six softkey buttons, left to right
    Soft2,
    Soft3,
    Soft4,
    Soft5,
    Soft6,
    TrackPrev,
    TrackNext,
    JumpSample,  ///< Root-menu jumps, one per group of ui-information-architecture.md §1
    JumpPlay,
    JumpInstrument,
    JumpTrack,
    JumpMixer,
    JumpSettings,
    PlayStop,  ///< Transport (Phase 2 semantics; forwarded to the page until then)
    Rec,
    Pad1,  ///< The 4x4 pad grid, row-major from the top left
    Pad2,
    Pad3,
    Pad4,
    Pad5,
    Pad6,
    Pad7,
    Pad8,
    Pad9,
    Pad10,
    Pad11,
    Pad12,
    Pad13,
    Pad14,
    Pad15,
    Pad16,
    Count
};

/// Console/log name: SOFT1, SHIFT, NAV_A_PUSH, TRACK_PREV, PAD16 ... The
/// spelling is the one panel-controls.md §3.4 uses. "NONE" for None.
const char* panelKeyName(PanelKey key);

/// Inverse of panelKeyName(), case-sensitive on the upper-case names, plus
/// the console's historical spellings SELECT (NavAPush) and ENC/CLICK
/// (NavBPush). PanelKey::None when the name is not a key.
PanelKey panelKeyFromName(const char* name);

/// 0..5 for Soft1..Soft6, -1 for anything else.
inline int softkeyIndex(PanelKey key) {
    const int i = static_cast<int>(key) - static_cast<int>(PanelKey::Soft1);
    return (i >= 0 && i < 6) ? i : -1;
}

/// 0..15 for Pad1..Pad16, -1 for anything else.
inline int padIndex(PanelKey key) {
    const int i = static_cast<int>(key) - static_cast<int>(PanelKey::Pad1);
    return (i >= 0 && i < 16) ? i : -1;
}

/// TCA8418 keycode (1..80, row * 10 + column + 1) to key, through the
/// WAVEX_KEYCODE_* map. PanelKey::None for a keycode nothing is wired to.
PanelKey panelKeyFromKeycode(uint8_t keycode);

}  // namespace wavex_ui
