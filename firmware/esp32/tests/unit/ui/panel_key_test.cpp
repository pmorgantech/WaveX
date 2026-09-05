// The panel key model (panel-controls.md §4.2): the name table the console's
// KEY verb and the Diagnostics ▸ Panel tab use, the WAVEX_KEYCODE_* map from
// hardware_config.h, and the LED channel map. All of it is host-testable
// because none of it touches the HAL; the static_asserts in panel_key.cpp
// and panel_led.h catch a duplicate or out-of-range entry at compile time,
// these tests catch the rest.

#include "ui/panel_key.h"

#include <gtest/gtest.h>

#include "config/hardware_config.h"
#include "ui/input_event.h"
#include "ui/panel_led.h"

#include <cstdint>
#include <cstring>
#include <set>
#include <string>

using wavex_ui::InputEvent;
using wavex_ui::InputType;
using wavex_ui::PanelKey;
using wavex_ui::PanelLed;

namespace {

constexpr int kKeyCount = static_cast<int>(PanelKey::Count);

TEST(PanelKeyNames, EveryKeyHasAUniqueName) {
    std::set<std::string> seen;
    for (int i = 0; i < kKeyCount; ++i) {
        const char* name = wavex_ui::panelKeyName(static_cast<PanelKey>(i));
        ASSERT_NE(name, nullptr);
        EXPECT_STRNE(name, "?") << "key " << i;
        EXPECT_TRUE(seen.insert(name).second) << "duplicate name " << name;
    }
    EXPECT_STREQ(wavex_ui::panelKeyName(PanelKey::Count), "?");
}

TEST(PanelKeyNames, RoundTrip) {
    for (int i = 1; i < kKeyCount; ++i) {
        const auto key = static_cast<PanelKey>(i);
        EXPECT_EQ(wavex_ui::panelKeyFromName(wavex_ui::panelKeyName(key)), key);
    }
}

TEST(PanelKeyNames, SpellingsTheDocUses) {
    EXPECT_STREQ(wavex_ui::panelKeyName(PanelKey::NavAPush), "NAV_A_PUSH");
    EXPECT_STREQ(wavex_ui::panelKeyName(PanelKey::Soft1), "SOFT1");
    EXPECT_STREQ(wavex_ui::panelKeyName(PanelKey::Soft6), "SOFT6");
    EXPECT_STREQ(wavex_ui::panelKeyName(PanelKey::TrackPrev), "TRACK_PREV");
    EXPECT_STREQ(wavex_ui::panelKeyName(PanelKey::JumpSample), "SAMPLE");
    EXPECT_STREQ(wavex_ui::panelKeyName(PanelKey::PlayStop), "PLAY_STOP");
    EXPECT_STREQ(wavex_ui::panelKeyName(PanelKey::Pad1), "PAD1");
    EXPECT_STREQ(wavex_ui::panelKeyName(PanelKey::Pad16), "PAD16");
}

// The console's KEY verb accepted these before the panel had names; the HIL
// suite still sends them.
TEST(PanelKeyNames, LegacyConsoleAliases) {
    EXPECT_EQ(wavex_ui::panelKeyFromName("SELECT"), PanelKey::NavAPush);
    EXPECT_EQ(wavex_ui::panelKeyFromName("ENC"), PanelKey::NavBPush);
    EXPECT_EQ(wavex_ui::panelKeyFromName("CLICK"), PanelKey::NavBPush);
    EXPECT_EQ(wavex_ui::panelKeyFromName("BACK"), PanelKey::Back);
    EXPECT_EQ(wavex_ui::panelKeyFromName("SHIFT"), PanelKey::Shift);
}

TEST(PanelKeyNames, UnknownIsNone) {
    EXPECT_EQ(wavex_ui::panelKeyFromName(nullptr), PanelKey::None);
    EXPECT_EQ(wavex_ui::panelKeyFromName(""), PanelKey::None);
    EXPECT_EQ(wavex_ui::panelKeyFromName("NONE"), PanelKey::None);
    EXPECT_EQ(wavex_ui::panelKeyFromName("soft1"), PanelKey::None);  // case-sensitive
    EXPECT_EQ(wavex_ui::panelKeyFromName("SOFT7"), PanelKey::None);
}

// The bench buttons' ids have been 1..4 since the first keypad build, and
// ui_softkey.h's BUTTON_* aliases are those numbers.
TEST(PanelKeyIds, BenchButtonsKeepTheirIds) {
    EXPECT_EQ(static_cast<uint8_t>(PanelKey::NavAPush), 1);
    EXPECT_EQ(static_cast<uint8_t>(PanelKey::Back), 2);
    EXPECT_EQ(static_cast<uint8_t>(PanelKey::NavBPush), 3);
    EXPECT_EQ(static_cast<uint8_t>(PanelKey::Shift), 4);
}

TEST(PanelKeyIndex, SoftkeysAndPads) {
    EXPECT_EQ(wavex_ui::softkeyIndex(PanelKey::Soft1), 0);
    EXPECT_EQ(wavex_ui::softkeyIndex(PanelKey::Soft6), 5);
    EXPECT_EQ(wavex_ui::softkeyIndex(PanelKey::Shift), -1);
    EXPECT_EQ(wavex_ui::softkeyIndex(PanelKey::TrackPrev), -1);
    EXPECT_EQ(wavex_ui::padIndex(PanelKey::Pad1), 0);
    EXPECT_EQ(wavex_ui::padIndex(PanelKey::Pad16), 15);
    EXPECT_EQ(wavex_ui::padIndex(PanelKey::Rec), -1);
    EXPECT_EQ(wavex_ui::padIndex(PanelKey::Count), -1);
}

// Keycodes the bench has confirmed (2026-09-04): the four buttons on the
// first row. The rest of the map is what the panel drawing says and is
// verified through the Diagnostics ▸ Panel tab.
TEST(PanelKeycodes, BenchConfirmedRowZero) {
    EXPECT_EQ(wavex_ui::panelKeyFromKeycode(WAVEX_KEYCODE_NAV_A_PUSH), PanelKey::NavAPush);
    EXPECT_EQ(wavex_ui::panelKeyFromKeycode(WAVEX_KEYCODE_BACK), PanelKey::Back);
    EXPECT_EQ(wavex_ui::panelKeyFromKeycode(WAVEX_KEYCODE_NAV_B_PUSH), PanelKey::NavBPush);
    EXPECT_EQ(wavex_ui::panelKeyFromKeycode(WAVEX_KEYCODE_SHIFT), PanelKey::Shift);
}

TEST(PanelKeycodes, EveryKeyMapsBackToItself) {
    std::set<uint8_t> seen;
    for (uint8_t code = 1; code <= 80; ++code) {
        const PanelKey key = wavex_ui::panelKeyFromKeycode(code);
        if (key != PanelKey::None) {
            EXPECT_TRUE(seen.insert(code).second);
        }
    }
    // 37 keys on the panel (panel-controls.md §3.4), 37 keycodes.
    EXPECT_EQ(seen.size(), static_cast<size_t>(kKeyCount - 1));
}

TEST(PanelKeycodes, UnwiredIsNone) {
    EXPECT_EQ(wavex_ui::panelKeyFromKeycode(0), PanelKey::None);
    EXPECT_EQ(wavex_ui::panelKeyFromKeycode(80), PanelKey::None);  // row 7 col 9: nothing there
    EXPECT_EQ(wavex_ui::panelKeyFromKeycode(255), PanelKey::None);
}

TEST(PanelLeds, ChannelsAreUniqueAndInRange) {
    std::set<uint8_t> seen;
    for (int i = 0; i < static_cast<int>(PanelLed::Count); ++i) {
        const uint8_t ch = wavex_ui::panelLedChannel(static_cast<PanelLed>(i));
        EXPECT_LT(ch, WAVEX_LED_CHANNELS);
        EXPECT_TRUE(seen.insert(ch).second) << "LED " << i << " shares channel " << int(ch);
    }
}

// InputEvent carries the key in source_id; key() decodes it for every key
// event type and refuses an id past the enum.
TEST(InputEventKey, DecodesKeyAndButtonEvents) {
    InputEvent evt{};
    evt.type = InputType::KeyPress;
    evt.source_id = static_cast<uint8_t>(PanelKey::JumpMixer);
    EXPECT_EQ(evt.key(), PanelKey::JumpMixer);
    EXPECT_TRUE(evt.isKeyPress());

    evt.type = InputType::ButtonPress;
    evt.source_id = static_cast<uint8_t>(PanelKey::Back);
    EXPECT_EQ(evt.key(), PanelKey::Back);
    EXPECT_TRUE(evt.isKeyPress());

    evt.type = InputType::KeyRelease;
    EXPECT_EQ(evt.key(), PanelKey::Back);
    EXPECT_FALSE(evt.isKeyPress());

    evt.source_id = static_cast<uint8_t>(PanelKey::Count);
    EXPECT_EQ(evt.key(), PanelKey::None);

    evt.type = InputType::EncoderRight;
    evt.source_id = 1;
    EXPECT_EQ(evt.key(), PanelKey::None);
    EXPECT_FALSE(evt.isKeyPress());
}

// The pots post like the encoders: a magnitude, the direction in the type.
TEST(InputEventSteps, PotsFollowTheEncoderContract) {
    InputEvent evt{};
    evt.delta = 4;
    evt.type = InputType::PotUp;
    EXPECT_EQ(evt.steps(), 4);
    evt.type = InputType::PotDown;
    EXPECT_EQ(evt.steps(), -4);
}

}  // namespace
