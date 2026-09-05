// WaveX panel keys: name table and the keycode map
#include "ui/panel_key.h"

#include "config/hardware_config.h"

#include <cstddef>
#include <cstring>

namespace wavex_ui {

namespace {

constexpr size_t kKeyCount = static_cast<size_t>(PanelKey::Count);

// Indexed by PanelKey. The order is the enum's; a key added to one without
// the other trips the size check below rather than shifting every name.
constexpr const char* kNames[kKeyCount] = {
    "NONE",  "NAV_A_PUSH", "BACK",     "NAV_B_PUSH", "SHIFT",      "SOFT1",  "SOFT2", "SOFT3",
    "SOFT4", "SOFT5",      "SOFT6",    "TRACK_PREV", "TRACK_NEXT", "SAMPLE", "PLAY",  "INSTRUMENT",
    "TRACK", "MIXER",      "SETTINGS", "PLAY_STOP",  "REC",        "PAD1",   "PAD2",  "PAD3",
    "PAD4",  "PAD5",       "PAD6",     "PAD7",       "PAD8",       "PAD9",   "PAD10", "PAD11",
    "PAD12", "PAD13",      "PAD14",    "PAD15",      "PAD16",
};
static_assert(sizeof(kNames) / sizeof(kNames[0]) == kKeyCount, "one name per PanelKey");

// Indexed by PanelKey: the keycode the matrix reports for it. The numbers are
// wiring truth and live in hardware_config.h; this only arranges them.
constexpr uint8_t kKeycodes[kKeyCount] = {
    0,  // None
    WAVEX_KEYCODE_NAV_A_PUSH,
    WAVEX_KEYCODE_BACK,
    WAVEX_KEYCODE_NAV_B_PUSH,
    WAVEX_KEYCODE_SHIFT,
    WAVEX_KEYCODE_SOFT1,
    WAVEX_KEYCODE_SOFT2,
    WAVEX_KEYCODE_SOFT3,
    WAVEX_KEYCODE_SOFT4,
    WAVEX_KEYCODE_SOFT5,
    WAVEX_KEYCODE_SOFT6,
    WAVEX_KEYCODE_TRACK_PREV,
    WAVEX_KEYCODE_TRACK_NEXT,
    WAVEX_KEYCODE_JUMP_SAMPLE,
    WAVEX_KEYCODE_JUMP_PLAY,
    WAVEX_KEYCODE_JUMP_INSTRUMENT,
    WAVEX_KEYCODE_JUMP_TRACK,
    WAVEX_KEYCODE_JUMP_MIXER,
    WAVEX_KEYCODE_JUMP_SETTINGS,
    WAVEX_KEYCODE_PLAY_STOP,
    WAVEX_KEYCODE_REC,
    WAVEX_KEYCODE_PAD1,
    WAVEX_KEYCODE_PAD2,
    WAVEX_KEYCODE_PAD3,
    WAVEX_KEYCODE_PAD4,
    WAVEX_KEYCODE_PAD5,
    WAVEX_KEYCODE_PAD6,
    WAVEX_KEYCODE_PAD7,
    WAVEX_KEYCODE_PAD8,
    WAVEX_KEYCODE_PAD9,
    WAVEX_KEYCODE_PAD10,
    WAVEX_KEYCODE_PAD11,
    WAVEX_KEYCODE_PAD12,
    WAVEX_KEYCODE_PAD13,
    WAVEX_KEYCODE_PAD14,
    WAVEX_KEYCODE_PAD15,
    WAVEX_KEYCODE_PAD16,
};

// The TCA8418 reports keycodes 1..80 (8 rows x 10 columns); 0 is "no key".
constexpr uint8_t kMaxKeycode = 80;

constexpr bool keycodesInRange() {
    for (size_t i = 1; i < kKeyCount; ++i) {
        if (kKeycodes[i] == 0 || kKeycodes[i] > kMaxKeycode) {
            return false;
        }
    }
    return true;
}

constexpr bool keycodesUnique() {
    for (size_t i = 1; i < kKeyCount; ++i) {
        for (size_t j = i + 1; j < kKeyCount; ++j) {
            if (kKeycodes[i] == kKeycodes[j]) {
                return false;
            }
        }
    }
    return true;
}

static_assert(keycodesInRange(), "every PanelKey needs a keycode in 1..80 (hardware_config.h)");
static_assert(keycodesUnique(), "two PanelKeys claim the same keycode (hardware_config.h)");

}  // namespace

const char* panelKeyName(PanelKey key) {
    const size_t i = static_cast<size_t>(key);
    return i < kKeyCount ? kNames[i] : "?";
}

PanelKey panelKeyFromName(const char* name) {
    if (!name || !*name) {
        return PanelKey::None;
    }
    // What the console called the bench buttons before the panel had names.
    if (!strcmp(name, "SELECT")) {
        return PanelKey::NavAPush;
    }
    if (!strcmp(name, "ENC") || !strcmp(name, "CLICK")) {
        return PanelKey::NavBPush;
    }
    for (size_t i = 1; i < kKeyCount; ++i) {
        if (!strcmp(name, kNames[i])) {
            return static_cast<PanelKey>(i);
        }
    }
    return PanelKey::None;
}

PanelKey panelKeyFromKeycode(uint8_t keycode) {
    // 37 entries; a linear scan per key event is nothing next to the I2C
    // read that produced it, and it keeps the wiring table the only table.
    if (keycode == 0) {
        return PanelKey::None;
    }
    for (size_t i = 1; i < kKeyCount; ++i) {
        if (kKeycodes[i] == keycode) {
            return static_cast<PanelKey>(i);
        }
    }
    return PanelKey::None;
}

}  // namespace wavex_ui
