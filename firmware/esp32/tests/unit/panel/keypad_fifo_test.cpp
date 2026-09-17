#include "ui/panel/keypad_fifo.h"

#include <gtest/gtest.h>

#include <array>
#include <deque>
#include <vector>
using wavex_ui::KeypadFifo;
namespace {
struct Device {
    std::array<uint8_t, 256> regs{};
    std::deque<uint8_t> events;
    std::vector<std::pair<uint8_t, uint8_t>> writes;
    bool fail = false, endless = false;
    uint8_t on_ack = 0;
    bool read(uint8_t reg, uint8_t& value) {
        if (fail)
            return false;
        if (reg == 3)
            value = endless ? 10 : static_cast<uint8_t>(events.size());
        else if (reg == 4) {
            value = endless ? 0x81 : events.empty() ? 0 : events.front();
            if (!events.empty())
                events.pop_front();
        } else
            value = regs[reg];
        return true;
    }
    bool write(uint8_t reg, uint8_t value) {
        if (fail)
            return false;
        writes.emplace_back(reg, value);
        if (reg == 2) {
            regs[reg] &= static_cast<uint8_t>(~value);
            if (on_ack) {
                events.push_back(on_ack);
                on_ack = 0;
            }
        } else
            regs[reg] = value;
        return true;
    }
};
struct Sink {
    std::vector<uint8_t> events;
    bool accept = true;
    bool operator()(bool pressed, uint8_t key) {
        if (!accept)
            return false;
        events.push_back(static_cast<uint8_t>(key | (pressed ? 0x80 : 0)));
        return true;
    }
};
}  // namespace
TEST(KeypadFifo, ConfiguresGeometryAndOverflowErratumWithoutGpioNoise) {
    KeypadFifo fifo;
    Device device;
    ASSERT_TRUE(fifo.configure(device, 8, 10));
    EXPECT_EQ(device.regs[1], 0x39);
    EXPECT_EQ(device.regs[0x1d], 255);
    EXPECT_EQ(device.regs[0x1e], 255);
    EXPECT_EQ(device.regs[0x1f], 3);
    EXPECT_EQ(device.regs[0x20], 0);
    EXPECT_EQ(device.regs[0x29], 0);
    EXPECT_TRUE(fifo.configure(device, 2, 4));
    EXPECT_EQ(device.regs[0x1f], 0);
    EXPECT_FALSE(fifo.configure(device, 0, 10));
    EXPECT_FALSE(fifo.configure(device, 8, 11));
    device.fail = true;
    EXPECT_FALSE(fifo.configure(device, 8, 10));
}
TEST(KeypadFifo, PreservesOrderedChordsAndEventArrivingDuringAck) {
    KeypadFifo fifo;
    Device d;
    Sink sink;
    d.regs[2] = 1;
    d.events = {0x81, 0x82, 1, 2};
    d.on_ack = 0x83;
    auto emit = [&](bool p, uint8_t k) { return sink(p, k); };
    EXPECT_EQ(fifo.service(d, emit), KeypadFifo::Result::More);
    EXPECT_EQ(sink.events, (std::vector<uint8_t>{0x81, 0x82, 1, 2}));
    EXPECT_EQ(fifo.service(d, emit), KeypadFifo::Result::Idle);
    EXPECT_EQ(sink.events.back(), 0x83);
}
TEST(KeypadFifo, BackpressureRetainsPressBeforeFollowingRelease) {
    KeypadFifo fifo;
    Device d;
    Sink sink;
    d.events = {0x81, 1};
    sink.accept = false;
    auto emit = [&](bool p, uint8_t k) { return sink(p, k); };
    EXPECT_EQ(fifo.service(d, emit), KeypadFifo::Result::More);
    EXPECT_EQ(d.events.size(), 1u);
    sink.accept = true;
    EXPECT_EQ(fifo.service(d, emit), KeypadFifo::Result::Idle);
    EXPECT_EQ(sink.events, (std::vector<uint8_t>{0x81, 1}));
}
TEST(KeypadFifo, OverflowAndIoFailureReleaseHeldNotesBeforeDiscardingAmbiguousHistory) {
    KeypadFifo fifo;
    Device d;
    Sink sink;
    auto emit = [&](bool p, uint8_t k) { return sink(p, k); };
    d.events = {0x81};
    fifo.service(d, emit);
    d.regs[2] = 9;
    d.events = {0x82, 1, 2};
    EXPECT_EQ(fifo.service(d, emit), KeypadFifo::Result::Idle);
    EXPECT_EQ(sink.events, (std::vector<uint8_t>{0x81, 1}));
    EXPECT_EQ(fifo.overflows(), 1u);
    d.events = {0x83};
    fifo.service(d, emit);
    d.fail = true;
    EXPECT_EQ(fifo.service(d, emit), KeypadFifo::Result::IoError);
    sink.accept = false;
    EXPECT_EQ(fifo.service(d, emit), KeypadFifo::Result::More);
    sink.accept = true;
    d.fail = false;
    EXPECT_EQ(fifo.service(d, emit), KeypadFifo::Result::Idle);
    EXPECT_EQ(sink.events.back(), 3);
    EXPECT_EQ(fifo.errors(), 1u);
}
TEST(KeypadFifo, BoundedDrainRejectsInvalidCodesAndStopRetriesReleases) {
    KeypadFifo fifo;
    Device d;
    Sink sink;
    auto emit = [&](bool p, uint8_t k) { return sink(p, k); };
    d.events = {0xff, 0x80, 0x81, 0x81};
    fifo.service(d, emit);
    EXPECT_EQ(fifo.invalid(), 2u);
    EXPECT_EQ(sink.events.size(), 1u);
    d.endless = true;
    EXPECT_EQ(fifo.service(d, emit), KeypadFifo::Result::More);
    sink.accept = false;
    EXPECT_FALSE(fifo.stop(emit));
    sink.accept = true;
    EXPECT_TRUE(fifo.stop(emit));
    EXPECT_EQ(sink.events.back(), 1);
}
