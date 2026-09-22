#include "comm/note_delivery.h"

#include <gtest/gtest.h>

#include <vector>
using namespace WaveX;
namespace {
Protocol::NoteMessage Note(uint8_t note = 60, uint8_t channel = 0) {
    Protocol::NoteMessage m;
    m.note = note;
    m.velocity = 100;
    m.channel = channel;
    m.reserved = 0;
    return m;
}
}  // namespace
TEST(NoteDelivery, RetriesReleaseBeforeAcceptingSameAddressRetrigger) {
    Comm::NoteDelivery delivery;
    bool ready = true;
    std::vector<uint8_t> types;
    auto send = [&](uint8_t type, const Protocol::NoteMessage&) {
        if (!ready)
            return false;
        types.push_back(type);
        return true;
    };
    ASSERT_TRUE(delivery.Send(Protocol::MSG_NOTE_ON, Note(), send));
    ready = false;
    EXPECT_TRUE(delivery.Send(Protocol::MSG_NOTE_OFF, Note(), send));
    delivery.Service(send);
    EXPECT_FALSE(delivery.Send(Protocol::MSG_NOTE_ON, Note(), send));
    ready = true;
    EXPECT_FALSE(delivery.Send(Protocol::MSG_NOTE_ON, Note(), send));
    delivery.Service(send);
    EXPECT_TRUE(delivery.Send(Protocol::MSG_NOTE_ON, Note(), send));
    EXPECT_EQ(types,
              (std::vector<uint8_t>{
                  Protocol::MSG_NOTE_ON, Protocol::MSG_NOTE_OFF, Protocol::MSG_NOTE_ON}));
}
TEST(NoteDelivery, RepeatedPressesRetainEveryReleaseAndAddress) {
    Comm::NoteDelivery delivery;
    bool ready = true;
    std::vector<uint8_t> released;
    auto send = [&](uint8_t type, const Protocol::NoteMessage& m) {
        if (!ready)
            return false;
        if (type == Protocol::MSG_NOTE_OFF)
            released.push_back(m.channel);
        return true;
    };
    const auto note = Note(62, Protocol::NoteChannelForTrack(3));
    for (int i = 0; i < 3; ++i)
        ASSERT_TRUE(delivery.Send(Protocol::MSG_NOTE_ON, note, send));
    ready = false;
    for (int i = 0; i < 3; ++i)
        ASSERT_TRUE(delivery.Send(Protocol::MSG_NOTE_OFF, note, send));
    ready = true;
    for (int i = 0; i < 4; ++i)
        delivery.Service(send);
    EXPECT_EQ(released, std::vector<uint8_t>(3, note.channel));
}
TEST(NoteDelivery, ReservesReleaseCapacityAndNeverReplaysRejectedNoteOn) {
    Comm::NoteDelivery delivery;
    bool ready = true;
    int on = 0, off = 0;
    auto send = [&](uint8_t type, const Protocol::NoteMessage&) {
        if (!ready)
            return false;
        if (type == Protocol::MSG_NOTE_ON)
            ++on;
        else
            ++off;
        return true;
    };
    for (size_t i = 0; i < Comm::NoteDelivery::kCapacity; ++i)
        ASSERT_TRUE(delivery.Send(Protocol::MSG_NOTE_ON, Note(static_cast<uint8_t>(i)), send));
    EXPECT_FALSE(delivery.Send(Protocol::MSG_NOTE_ON, Note(0, 1), send));
    ready = false;
    for (size_t i = 0; i < Comm::NoteDelivery::kCapacity; ++i)
        ASSERT_TRUE(delivery.Send(Protocol::MSG_NOTE_OFF, Note(static_cast<uint8_t>(i)), send));
    ready = true;
    for (int i = 0; i < 20; ++i)
        delivery.Service(send);
    EXPECT_EQ(on, 128);
    EXPECT_EQ(off, 128);
}
