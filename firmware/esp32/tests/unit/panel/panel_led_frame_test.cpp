#include "ui/panel/panel_led_frame.h"

#include <gtest/gtest.h>

#include "panel/tlc5947_frame.h"

#include <array>
using namespace wavex_ui;
namespace {
uint8_t level(const PanelLedFrame& frame, PanelLed led) {
    return frame.levels[static_cast<size_t>(led)];
}
uint16_t decode(const std::array<uint8_t, wavex_panel::kTlcFrameBytes>& bytes, size_t channel) {
    const size_t word = WAVEX_LED_CHANNELS - 1 - channel;
    const size_t offset = word / 2 * 3;
    return static_cast<uint16_t>(word % 2 == 0
                                     ? (bytes[offset] << 4) | (bytes[offset + 1] >> 4)
                                     : ((bytes[offset + 1] & 15) << 8) | bytes[offset + 2]);
}
}  // namespace
TEST(PanelLedFrame, PolicyUsesExplicitStateAndClearsUnrelatedContext) {
    PanelLedInputs in;
    in.root = RootGroup::Play;
    in.shifted = true;
    in.playing = true;
    in.soft_defined[0] = in.soft_defined[1] = in.soft_defined[2] = true;
    in.soft_enabled[0] = in.soft_enabled[1] = true;
    in.soft_active[1] = in.soft_active[2] = true;
    in.pads = {0x8003, 0x8002, true};
    auto frame = BuildPanelLedFrame(in);
    EXPECT_EQ(level(frame, PanelLed::Soft1), 32);
    EXPECT_EQ(level(frame, PanelLed::Soft2), 192);
    EXPECT_EQ(level(frame, PanelLed::Soft3), 32);  // disabled active remains dim
    EXPECT_EQ(level(frame, PanelLed::Soft4), 0);
    EXPECT_EQ(level(frame, PanelLed::Shift), 192);
    EXPECT_EQ(level(frame, PanelLed::JumpPlay), 192);
    EXPECT_EQ(level(frame, PanelLed::JumpSample), 0);
    EXPECT_EQ(level(frame, PanelLed::PlayStop), 192);
    EXPECT_EQ(level(frame, PanelLed::Rec), 192);
    EXPECT_EQ(level(frame, PanelLed::Pad1), 32);
    EXPECT_EQ(level(frame, PanelLed::Pad2), 192);
    EXPECT_EQ(level(frame, PanelLed::Pad16), 192);
    frame = BuildPanelLedFrame({});
    for (auto value: frame.levels)
        EXPECT_EQ(value, 0);
}
TEST(PanelLedFrame, OnlyTheMatchingRootIsLit) {
    const RootGroup roots[]{RootGroup::Sample,
                            RootGroup::Play,
                            RootGroup::Instrument,
                            RootGroup::Track,
                            RootGroup::Mixer,
                            RootGroup::Settings};
    for (size_t r = 0; r < 6; ++r) {
        PanelLedInputs in;
        in.root = roots[r];
        auto frame = BuildPanelLedFrame(in);
        for (size_t i = 0; i < 6; ++i)
            EXPECT_EQ(frame.levels[static_cast<size_t>(PanelLed::JumpSample) + i],
                      r == i ? 192 : 0);
    }
}
TEST(PanelLedFrame, EachPhysicalWalkHasExactlyOneFullScaleOutputInWireOrder) {
    PanelLedFrame frame;
    frame.blanked = false;
    std::array<uint8_t, wavex_panel::kTlcFrameBytes> bytes;
    for (int ch = 0; ch < WAVEX_LED_CHANNELS; ++ch) {
        frame.test_channel = static_cast<int16_t>(ch);
        wavex_panel::EncodeTlc5947(frame, bytes.data());
        for (size_t i = 0; i < WAVEX_LED_CHANNELS; ++i)
            EXPECT_EQ(decode(bytes, i), i == size_t(ch) ? 4095 : 0);
    }
}
TEST(PanelLedFrame, LogicalMapAndBrightnessRoundTripWithUnusedOutputsDark) {
    PanelLedFrame frame;
    frame.blanked = false;
    std::array<uint8_t, wavex_panel::kTlcFrameBytes> bytes;
    for (size_t led = 0; led < kPanelLedCount; ++led) {
        for (uint8_t brightness:
             {uint8_t(0), uint8_t(1), uint8_t(32), uint8_t(192), uint8_t(255)}) {
            frame.levels.fill(0);
            frame.levels[led] = brightness;
            wavex_panel::EncodeTlc5947(frame, bytes.data());
            for (size_t ch = 0; ch < WAVEX_LED_CHANNELS; ++ch)
                EXPECT_EQ(decode(bytes, ch),
                          ch == panelLedChannel(static_cast<PanelLed>(led))
                              ? (uint32_t(brightness) * 4095 + 127) / 255
                              : 0);
        }
    }
}
TEST(PanelLedFrame, BlankingAndExpiredHeartbeatOverrideDiagnosticAllOnAcrossClockWrap) {
    PanelLedFrame frame;
    frame.blanked = false;
    frame.test_all = true;
    std::array<uint8_t, wavex_panel::kTlcFrameBytes> bytes;
    wavex_panel::EncodeTlc5947(frame, bytes.data());
    for (auto b: bytes)
        EXPECT_EQ(b, 255);
    EXPECT_FALSE(FreshPanelLedFrame(frame, 100, UINT32_MAX - 899).blanked);
    frame = FreshPanelLedFrame(frame, 101, UINT32_MAX - 899);
    EXPECT_TRUE(frame.blanked);
    wavex_panel::EncodeTlc5947(frame, bytes.data());
    for (auto b: bytes)
        EXPECT_EQ(b, 0);
    PanelLedInputs in;
    in.blanked = true;
    in.pads.active = 0xffff;
    for (auto b: BuildPanelLedFrame(in).levels)
        EXPECT_EQ(b, 0);
}
