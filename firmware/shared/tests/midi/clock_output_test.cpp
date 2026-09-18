#include "midi/clock_output.hpp"

#include <gtest/gtest.h>
using namespace WaveX::Midi;
using namespace WaveX::Protocol;
TEST(MidiClockOutput, WireAndUsbPacketsMatchMidiAndRejectMalformedMessages) {
    constexpr uint8_t statuses[]{0xf8, 0xfa, 0xfb, 0xfc, 0xf2};
    for (uint8_t event = 0; event < 5; ++event) {
        ClockPacket packet;
        ASSERT_TRUE(
            EncodeClock({event, 65535, static_cast<uint16_t>(event == 4 ? 0x1234 : 0)}, packet));
        EXPECT_EQ(packet.bytes[0], statuses[event]);
        EXPECT_EQ(packet.size, event == 4 ? 3 : 1);
        const auto usb = packet.Usb();
        EXPECT_EQ(usb[0], event == 4 ? 3 : 15);
        EXPECT_EQ(usb[1], statuses[event]);
        EXPECT_EQ(usb[2], event == 4 ? 0x34 : 0);
        EXPECT_EQ(usb[3], event == 4 ? 0x24 : 0);
    }
    ClockPacket packet;
    for (auto message: {SeqClockOutMessage(5, 0, 0),
                        SeqClockOutMessage(0, 0, 1),
                        SeqClockOutMessage(4, 0, 16384)})
        EXPECT_FALSE(EncodeClock(message, packet));
    SeqClockOutMessage message;
    message.reserved = 1;
    EXPECT_FALSE(EncodeClock(message, packet));
    message.reserved = 0;
    message.reserved2 = 1;
    EXPECT_FALSE(EncodeClock(message, packet));
}
TEST(MidiClockOutput, BackpressureExpiryWrapAndReconnectDoNotReplayOldTicks) {
    ClockOutputQueue queue;
    ClockPacket tick, out;
    ASSERT_TRUE(EncodeClock({MIDI_CLK_TICK, 0, 0}, tick));
    EXPECT_FALSE(queue.Push(tick, 0));
    queue.Ready(true);
    for (size_t i = 0; i < queue.kCapacity; ++i)
        ASSERT_TRUE(queue.Push(tick, UINT32_MAX - 10));
    EXPECT_FALSE(queue.Push(tick, 0));
    EXPECT_TRUE(queue.Pop(5, out));
    queue.Complete(false);
    EXPECT_FALSE(queue.Pop(60, out));
    EXPECT_EQ(queue.Stats().expired, 31u);
    EXPECT_EQ(queue.Stats().failed, 1u);
    ASSERT_TRUE(queue.Push(tick, 100));
    queue.Ready(false);
    queue.Ready(true);
    EXPECT_FALSE(queue.Pop(101, out));
}
TEST(MidiClockOutput, StopAndStartDiscardBacklogButPositionContinueRetainOrder) {
    ClockOutputQueue queue;
    queue.Ready(true);
    ClockPacket packet, out;
    for (auto event: {MIDI_CLK_TICK, MIDI_CLK_SPP, MIDI_CLK_CONTINUE}) {
        ASSERT_TRUE(EncodeClock(
            {static_cast<uint8_t>(event), 0, static_cast<uint16_t>(event == MIDI_CLK_SPP ? 22 : 0)},
            packet));
        ASSERT_TRUE(queue.Push(packet, 10));
    }
    for (auto expected: {0xf8, 0xf2, 0xfb}) {
        ASSERT_TRUE(queue.Pop(10, out));
        EXPECT_EQ(out.bytes[0], expected);
    }
    for (auto boundary: {MIDI_CLK_START, MIDI_CLK_STOP}) {
        ASSERT_TRUE(EncodeClock({MIDI_CLK_TICK, 0, 0}, packet));
        ASSERT_TRUE(queue.Push(packet, 20));
        ASSERT_TRUE(EncodeClock({static_cast<uint8_t>(boundary), 0, 0}, packet));
        ASSERT_TRUE(queue.Push(packet, 20));
        ASSERT_TRUE(queue.Pop(boundary == MIDI_CLK_STOP ? 1000 : 20, out));
        EXPECT_EQ(out.bytes[0], boundary == MIDI_CLK_START ? 0xfa : 0xfc);
        EXPECT_FALSE(queue.Pop(20, out));
    }
}
