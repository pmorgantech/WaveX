#include <gtest/gtest.h>

#include "spi_protocol/protocol.h"

#include <array>
using namespace WaveX::Protocol;
TEST(MidiPressureProtocol, RoundTripAndValidation) {
    for (uint8_t channel = 0; channel < 16; ++channel) {
        MidiPressureMessage in{127, channel}, out;
        ASSERT_TRUE(IsValidMidiPressure(in));
        std::array<uint8_t, 32> packet{};
        ASSERT_EQ(ProtocolHandler::CreatePacket(
                      packet.data(), packet.size(), MSG_MIDI_PRESSURE, &in, sizeof(in)),
                  packet.size());
        ASSERT_TRUE(
            ProtocolHandler::ParseMessage(packet.data(), MSG_MIDI_PRESSURE, &out, sizeof(out)));
        EXPECT_EQ(out.channel, channel);
        EXPECT_EQ(out.value, 127);
    }
    EXPECT_FALSE(IsValidMidiPressure({128, 0}));
    EXPECT_FALSE(IsValidMidiPressure({1, 16}));
    EXPECT_TRUE(IsValidMidiCc({1, 127, 15}));
    EXPECT_FALSE(IsValidMidiCc({128, 1, 0}));
    EXPECT_FALSE(IsValidMidiCc({1, 128, 0}));
    EXPECT_FALSE(IsValidMidiCc({1, 0, 16}));
    MidiCcMessage cc;
    cc.reserved = 1;
    EXPECT_FALSE(IsValidMidiCc(cc));
}
