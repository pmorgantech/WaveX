#include <gtest/gtest.h>

#include "midi_out.h"
#include "packet_router.h"
#include "test_helpers.h"
using namespace WaveX::Protocol;
using namespace wavex_midi;
class MidiOutputRoute : public ::testing::Test {
   protected:
    WaveX::Comm::PacketRouter router;
    void SetUp() override {
        Ready(Port::Din, false);
        Ready(Port::Usb, false);
        Ready(Port::Din, true);
        Ready(Port::Usb, true);
    }
    void TearDown() override {
        Ready(Port::Din, false);
        Ready(Port::Usb, false);
    }
};
TEST_F(MidiOutputRoute, UartAndUnifiedClockMessagesReachIndependentQueues) {
    SeqClockOutMessage message(MIDI_CLK_SPP, 5, 0x321);
    router.route_uart_message(
        MSG_SEQ_CLOCK_OUT, reinterpret_cast<uint8_t*>(&message), sizeof(message), 0, 1);
    WaveX::Midi::ClockPacket packet;
    ASSERT_TRUE(Take(Port::Din, packet));
    EXPECT_EQ(packet.bytes[1], 0x21);
    EXPECT_EQ(packet.bytes[2], 6);
    ASSERT_TRUE(Take(Port::Usb, packet));
    EXPECT_EQ(packet.Usb()[0], 3);
    message = {MIDI_CLK_START, 6, 0};
    auto wire = WaveX::Test::ProtocolTestHelper::CreateWaveXPacket(
        MSG_SEQ_CLOCK_OUT, &message, sizeof(message));
    router.route_packet(wire.data(), wire.size());
    ASSERT_TRUE(Take(Port::Usb, packet));
    EXPECT_EQ(packet.bytes[0], 0xfa);
    ASSERT_TRUE(Take(Port::Din, packet));
    EXPECT_EQ(packet.bytes[0], 0xfa);
}
TEST_F(MidiOutputRoute, InvalidPayloadsAcksAndUnavailablePortCannotLeakMessages) {
    SeqClockOutMessage message;
    auto* bytes = reinterpret_cast<uint8_t*>(&message);
    router.route_uart_message(MSG_SEQ_CLOCK_OUT, nullptr, sizeof(message), 0, 1);
    router.route_uart_message(MSG_SEQ_CLOCK_OUT, bytes, sizeof(message) - 1, 0, 1);
    router.route_uart_message(MSG_SEQ_CLOCK_OUT, bytes, sizeof(message) + 1, 0, 1);
    router.route_uart_message(MSG_SEQ_CLOCK_OUT, bytes, sizeof(message), PKT_FLAG_ACK, 1);
    message.reserved = 1;
    router.route_uart_message(MSG_SEQ_CLOCK_OUT, bytes, sizeof(message), 0, 1);
    uint8_t padded[9]{};
    padded[8] = 1;
    auto wire = WaveX::Test::ProtocolTestHelper::CreateWaveXPacket(
        MSG_SEQ_CLOCK_OUT, padded, sizeof(padded));
    router.route_packet(wire.data(), wire.size());
    WaveX::Midi::ClockPacket packet;
    EXPECT_FALSE(Take(Port::Din, packet));
    EXPECT_FALSE(Take(Port::Usb, packet));
    Ready(Port::Usb, false);
    message = {MIDI_CLK_TICK, 0, 0};
    EXPECT_EQ(SendClock(message), kDin);
    EXPECT_TRUE(Take(Port::Din, packet));
    EXPECT_FALSE(Take(Port::Usb, packet));
    Ready(Port::Usb, true);
    EXPECT_FALSE(Take(Port::Usb, packet));
}
