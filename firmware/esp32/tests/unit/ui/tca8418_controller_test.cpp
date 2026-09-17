#include "ui/tca8418_controller.h"

#include <gtest/gtest.h>

#include <array>
#include <deque>
#include <utility>
#include <vector>

namespace {
using Controller = wavex_ui::Tca8418Controller;
using State = Controller::State;
using Event = std::pair<bool, uint8_t>;

// Models the silicon FIFO, sticky W1C status, and an event racing an ACK.
struct Bus {
    std::array<uint8_t, 256> regs{};
    std::deque<uint8_t> fifo;
    std::vector<std::pair<uint8_t, uint8_t>> writes;
    int fail_reg = -1;
    bool fail_write = false, continuous = false, corrupt_count = false;
    uint8_t on_ack = 0;
    unsigned reads = 0;
    bool read(uint8_t reg, uint8_t& value) {
        ++reads;
        if (reg == fail_reg && !fail_write)
            return false;
        if (reg == 0x03) {
            value = corrupt_count ? 15 : static_cast<uint8_t>(fifo.size());
        } else if (reg == 0x04) {
            value = fifo.empty() ? 0 : fifo.front();
            if (!fifo.empty())
                fifo.pop_front();
            if (continuous)
                fifo.push_back(value);
        } else {
            value = regs[reg];
        }
        return true;
    }
    bool write(uint8_t reg, uint8_t value) {
        if (reg == fail_reg && fail_write)
            return false;
        writes.emplace_back(reg, value);
        if (reg == 0x02) {
            regs[reg] &= static_cast<uint8_t>(~value);
            if (on_ack) {
                fifo.push_back(on_ack);
                regs[reg] |= 1;
                on_ack = 0;
            }
        } else {
            regs[reg] = value;
        }
        return true;
    }
};
struct Tca8418ControllerTest : testing::Test {
    Bus bus;
    Controller controller;
    std::vector<Event> events;
    auto sink() {
        return [this](bool press, uint8_t key) { events.emplace_back(press, key); };
    }
    Controller::Result service() { return controller.service(bus, sink()); }
};

TEST_F(Tca8418ControllerTest, MatrixConfigurationExcludesUnusedPinsAndRetainsDebounce) {
    bus.regs.fill(0xFF);
    ASSERT_TRUE(controller.init(bus, 4, 6));
    EXPECT_EQ(bus.regs[0x01], 0);
    EXPECT_EQ(bus.regs[0x1D], 0x0F);
    EXPECT_EQ(bus.regs[0x1E], 0x3F);
    EXPECT_EQ(bus.regs[0x1F], 0);
    EXPECT_EQ(bus.regs[0x20], 0);
    EXPECT_EQ(bus.regs[0x1A], 0);
    EXPECT_EQ(bus.regs[0x29], 0);
    ASSERT_TRUE(controller.init(bus, 8, 10));
    EXPECT_EQ(bus.regs[0x1D], 0xFF);
    EXPECT_EQ(bus.regs[0x1E], 0xFF);
    EXPECT_EQ(bus.regs[0x1F], 3);
}
TEST_F(Tca8418ControllerTest, InvalidGeometryDoesNotTouchHardware) {
    EXPECT_FALSE(controller.init(bus, 0, 8));
    EXPECT_FALSE(controller.init(bus, 9, 8));
    EXPECT_FALSE(controller.init(bus, 8, 0));
    EXPECT_FALSE(controller.init(bus, 8, 11));
    EXPECT_TRUE(bus.writes.empty());
}
TEST_F(Tca8418ControllerTest, InitAndInterruptReadbackFailuresPropagate) {
    bus.fail_reg = 0x1E;
    bus.fail_write = true;
    EXPECT_FALSE(controller.init(bus, 8, 10));
    bus.fail_reg = 1;
    EXPECT_FALSE(controller.interrupts(bus, true));
    bus.fail_write = false;
    EXPECT_FALSE(controller.interrupts(bus, true));
    bus.fail_reg = -1;
    ASSERT_TRUE(controller.interrupts(bus, true));
    EXPECT_EQ(bus.regs[1], 0x19);
    ASSERT_TRUE(controller.interrupts(bus, false));
    EXPECT_EQ(bus.regs[1], 0);
}
TEST_F(Tca8418ControllerTest, DrainsPressReleaseInOrderWithoutRequiringAnInterrupt) {
    bus.fifo = {0x81, 0xD0, 0x01, 0x50};
    EXPECT_EQ(service().state, State::Idle);
    EXPECT_EQ(events, (std::vector<Event>{{true, 1}, {true, 80}, {false, 1}, {false, 80}}));
    EXPECT_TRUE(bus.fifo.empty());
    EXPECT_TRUE(bus.writes.empty());
}
TEST_F(Tca8418ControllerTest, AcknowledgesOnlyObservedKeyFlagsAfterDraining) {
    bus.regs[2] = 0x03;  // GPI status does not belong to the matrix owner.
    bus.fifo = {0x81, 0x01};
    EXPECT_EQ(service().state, State::Idle);
    ASSERT_EQ(bus.writes.size(), 1u);
    EXPECT_EQ(bus.writes[0], (std::pair<uint8_t, uint8_t>{2, 1}));
    EXPECT_EQ(bus.regs[2], 2);
}
TEST_F(Tca8418ControllerTest, EventArrivingDuringAcknowledgeIsPreservedAndRequestsAnotherPass) {
    bus.regs[2] = 1;
    bus.fifo = {0x81};
    bus.on_ack = 0x01;
    EXPECT_EQ(service().state, State::Pending);
    ASSERT_EQ(bus.fifo.size(), 1u);
    EXPECT_EQ(service().state, State::Idle);
    EXPECT_EQ(events, (std::vector<Event>{{true, 1}, {false, 1}}));
}
TEST_F(Tca8418ControllerTest, ContinuousEventsAreCappedWithoutAcknowledgingUndrainedFifo) {
    bus.regs[2] = 1;
    bus.fifo = {0x81};
    bus.continuous = true;
    const auto result = service();
    EXPECT_EQ(result.state, State::Pending);
    EXPECT_EQ(result.events, Controller::kMaxEventsPerPass);
    EXPECT_EQ(events.size(), Controller::kMaxEventsPerPass);
    EXPECT_LE(bus.reads, 35u);
    EXPECT_TRUE(bus.writes.empty());
}
TEST_F(Tca8418ControllerTest, OverflowReleasesReportedKeysAndClearsOverflowFlag) {
    bus.fifo = {0x81};
    service();
    bus.regs[2] = 0x09;
    bus.fifo = {0x82};
    const auto result = service();
    EXPECT_TRUE(result.overflow);
    EXPECT_EQ(result.state, State::Idle);
    EXPECT_EQ(events, (std::vector<Event>{{true, 1}, {true, 2}, {false, 1}, {false, 2}}));
    EXPECT_EQ(bus.regs[2], 0);
    controller.releaseHeld(sink());
    EXPECT_EQ(events.size(), 4u);
}
TEST_F(Tca8418ControllerTest, ReadFailureReleasesHeldKeysWithoutConsumingPendingEvent) {
    bus.fifo = {0x81};
    service();
    bus.fifo = {0x82};
    bus.fail_reg = 4;
    EXPECT_EQ(service().state, State::IoError);
    EXPECT_EQ(events, (std::vector<Event>{{true, 1}, {false, 1}}));
    ASSERT_EQ(bus.fifo.size(), 1u);
    bus.fail_reg = -1;
    EXPECT_EQ(service().state, State::Idle);
    EXPECT_EQ(events.back(), (Event{true, 2}));
}
TEST_F(Tca8418ControllerTest, AcknowledgeFailureDoesNotLoseOrReplayConsumedEvents) {
    bus.regs[2] = 1;
    bus.fifo = {0x81, 1};
    bus.fail_reg = 2;
    bus.fail_write = true;
    EXPECT_EQ(service().state, State::IoError);
    bus.fail_reg = -1;
    EXPECT_EQ(service().state, State::Idle);
    EXPECT_EQ(events, (std::vector<Event>{{true, 1}, {false, 1}}));
}
TEST_F(Tca8418ControllerTest, InvalidDataDoesNotIndexOutsideTheMatrix) {
    for (uint8_t event: std::array<uint8_t, 4>{0, 0x80, 0xD1, 0xFF}) {
        bus.fifo = {event};
        EXPECT_EQ(service().state, State::InvalidEvent);
    }
    bus.corrupt_count = true;
    EXPECT_EQ(service().state, State::InvalidEvent);
    EXPECT_TRUE(events.empty());
}
TEST_F(Tca8418ControllerTest, StopReleasesOnlyKeysStillHeld) {
    bus.fifo = {0x81, 0x82, 0x01};
    service();
    controller.releaseHeld(sink());
    EXPECT_EQ(events, (std::vector<Event>{{true, 1}, {true, 2}, {false, 1}, {false, 2}}));
    controller.releaseHeld(sink());
    EXPECT_EQ(events.size(), 4u);
}
}  // namespace
