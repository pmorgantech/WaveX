#include <gtest/gtest.h>

#include "ui/pca9956b_controller.h"

#include <algorithm>

namespace {
using namespace wavex_ui;
using State = Pca9956bController::State;
struct LedBus {
    std::array<std::array<uint8_t, 128>, 2> registers{};
    bool blanked = true, reset_low = false, fail_next = false;
    unsigned transactions = 0;
    bool blank(bool value) {
        blanked = value;
        return true;
    }
    bool reset(bool asserted) {
        reset_low = asserted;
        if (asserted) {
            for (auto& device: registers) {
                device.fill(0);
                device[0] = 0x01;  // All-call enabled at POR.
                device[1] = 0x04;
            }
        }
        return true;
    }
    bool transfer() {
        ++transactions;
        if (fail_next) {
            fail_next = false;
            return false;
        }
        return true;
    }
    bool write(unsigned device, uint8_t reg, const uint8_t* data, size_t count) {
        if (!transfer())
            return false;
        std::copy_n(data, count, registers[device].begin() + reg);
        if (reg == 1)
            registers[device][1] = static_cast<uint8_t>((data[0] & ~0x10u) | 0x04);
        return true;
    }
    bool read(unsigned device, uint8_t reg, uint8_t* data, size_t count) {
        if (!transfer())
            return false;
        std::copy_n(registers[device].begin() + reg, count, data);
        return true;
    }
};
struct PanelLedDriverTest : testing::Test {
    LedBus bus;
    Pca9956bController driver{1, 32};
    PanelLedFrame desired;
    uint32_t now = 0;
    void step() {
        const auto before = bus.transactions;
        driver.service(bus, desired, now++);
        EXPECT_LE(bus.transactions - before, 1u);
    }
    void run(unsigned count) {
        while (count--)
            step();
    }
    void light() {
        desired.blank = false;
        desired.levels[panelLedChannel(PanelLed::Shift)] = 100;
        run(40);
    }
};
TEST(PanelLedPolicy, InitialPopulationHasFourteenUniqueOutputsOnFirstDevice) {
    constexpr auto used = panelLedPopulation();
    EXPECT_EQ(std::count(used.begin(), used.end(), true), 14);
    EXPECT_EQ(std::count(used.begin() + 24, used.end(), true), 0);
    EXPECT_FALSE(used[panelLedChannel(PanelLed::JumpMixer)]);
    EXPECT_FALSE(used[panelLedChannel(PanelLed::PlayStop)]);
}
TEST(PanelLedPolicy, EveryVisibleMenuEntryHasMatchingJumpAndIndicator) {
    for (const auto& item: kPanelMenuButtons) {
        if (!item.purpose)
            continue;
        EXPECT_EQ(panelMenuGroup(item.key), item.group);
        PanelLedInputs input;
        input.root = item.group;
        input.available_roots = 1u << static_cast<unsigned>(item.group);
        const auto frame = makePanelLedFrame(input);
        EXPECT_EQ(frame.levels[panelLedChannel(item.led)], WAVEX_PANEL_LED_BRIGHT);
        EXPECT_EQ(std::count_if(
                      frame.levels.begin(), frame.levels.end(), [](uint8_t v) { return v != 0; }),
                  1);
        input.available_roots = 0;
        EXPECT_EQ(makePanelLedFrame(input).levels[panelLedChannel(item.led)], 0);
    }
}
TEST(PanelLedPolicy, ShiftIsHeldOnlyAndSoftkeysRespectEnabledAndActiveState) {
    PanelLedInputs input;
    input.enabled_softkeys = 0x07;
    input.active_softkeys = 0x02;
    input.held_buttons = panelButtonBit(PanelKey::Shift) | panelButtonBit(PanelKey::Soft3) |
                         panelButtonBit(PanelKey::Soft4);
    auto frame = makePanelLedFrame(input);
    EXPECT_EQ(frame.levels[panelLedChannel(PanelLed::Shift)], WAVEX_PANEL_LED_BRIGHT);
    EXPECT_EQ(frame.levels[panelLedChannel(PanelLed::Soft1)], WAVEX_PANEL_LED_DIM);
    EXPECT_EQ(frame.levels[panelLedChannel(PanelLed::Soft2)], WAVEX_PANEL_LED_BRIGHT);
    EXPECT_EQ(frame.levels[panelLedChannel(PanelLed::Soft3)], WAVEX_PANEL_LED_BRIGHT);
    EXPECT_EQ(frame.levels[panelLedChannel(PanelLed::Soft4)], 0);
    input.held_buttons = 0;
    frame = makePanelLedFrame(input);
    EXPECT_EQ(frame.levels[panelLedChannel(PanelLed::Shift)], 0);
    EXPECT_EQ(frame.levels[panelLedChannel(PanelLed::Soft3)], WAVEX_PANEL_LED_DIM);
    input.blank = true;
    frame = makePanelLedFrame(input);
    EXPECT_TRUE(frame.blank);
    EXPECT_EQ(std::count(frame.levels.begin(), frame.levels.end(), 0), WAVEX_LED_CHANNELS);
}
TEST_F(PanelLedDriverTest, StartupStaysDarkUntilResetSettlesAndFrameIsInstalled) {
    desired.blank = false;
    desired.levels.fill(255);  // Reserved outputs must be sanitized even for malformed callers.
    step();
    EXPECT_TRUE(bus.blanked);
    EXPECT_TRUE(bus.reset_low);
    EXPECT_EQ(bus.transactions, 0u);
    step();
    EXPECT_FALSE(bus.reset_low);
    EXPECT_EQ(bus.transactions, 0u);
    step();
    EXPECT_EQ(bus.transactions, 0u);
    run(40);
    EXPECT_FALSE(bus.blanked);
    EXPECT_EQ(driver.state(), State::Ready);
    const auto used = panelLedPopulation();
    for (unsigned i = 0; i < 24; ++i) {
        EXPECT_EQ(bus.registers[0][0x22 + i], used[i] ? 32 : 0);
        EXPECT_EQ(bus.registers[0][0x0A + i], used[i] ? 255 : 0);
        EXPECT_EQ((bus.registers[0][2 + i / 4] >> (2 * (i % 4))) & 3, used[i] ? 2 : 0);
    }
    EXPECT_EQ(bus.registers[0][0], 0);
    const auto transfers = bus.transactions;
    run(100);
    EXPECT_EQ(bus.transactions, transfers);  // No writes for unchanged frames.
}
TEST_F(PanelLedDriverTest, CurrentUnsetDoesNotDriveOrPollTheBus) {
    Pca9956bController unset{1, 0};
    desired.blank = false;
    for (unsigned i = 0; i < 2000; ++i)
        unset.service(bus, desired, i);
    EXPECT_EQ(unset.state(), State::NeedsCurrent);
    EXPECT_TRUE(bus.blanked);
    EXPECT_EQ(bus.transactions, 0u);
}
TEST_F(PanelLedDriverTest, FaultBlanksImmediatelyAndRetriesWithoutBusyLoop) {
    light();
    ASSERT_FALSE(bus.blanked);
    desired.levels[0] = 123;
    step();  // Queue new frame.
    bus.fail_next = true;
    step();
    ASSERT_EQ(driver.state(), State::Retry);
    EXPECT_TRUE(bus.blanked);
    const auto transfers = bus.transactions;
    run(900);
    EXPECT_EQ(bus.transactions, transfers);
    desired.levels[0] = 77;
    run(150);
    EXPECT_EQ(driver.state(), State::Ready);
    EXPECT_EQ(bus.registers[0][0x0A], 77);
    EXPECT_FALSE(bus.blanked);
    EXPECT_EQ(driver.errors(), 1u);
}
TEST_F(PanelLedDriverTest, HealthCheckDetectsThermalFaultAndUnexpectedReset) {
    light();
    bus.registers[0][1] |= 0x80;
    run(1000);
    EXPECT_EQ(driver.state(), State::Retry);
    EXPECT_EQ(driver.faultStatus(), 0x80);
    EXPECT_TRUE(bus.blanked);
    run(1100);
    ASSERT_EQ(driver.state(), State::Ready);
    bus.registers[0][0] = 1;
    run(1000);
    EXPECT_EQ(driver.state(), State::Retry);
    EXPECT_TRUE(bus.blanked);
}
TEST_F(PanelLedDriverTest, BlankingDuringTransferCannotExposeAnOldFrameOnWake) {
    light();
    desired.levels[0] = 40;
    step();
    desired.blank = true;
    step();
    EXPECT_TRUE(bus.blanked);
    // Wake to exactly the in-flight frame: no dirty-frame trigger remains.
    desired.blank = false;
    step();
    EXPECT_FALSE(bus.blanked);
    desired.blank = true;
    run(30);
    EXPECT_TRUE(bus.blanked);
    desired.blank = false;
    desired.levels[0] = 80;
    run(30);
    EXPECT_FALSE(bus.blanked);
    EXPECT_EQ(bus.registers[0][0x0A], 80);
    driver.stop(bus);
    EXPECT_TRUE(bus.blanked);
    EXPECT_EQ(driver.state(), State::Off);
}
TEST(PanelLedDriver, TwoDevicesKeepOneSnapshotAcrossPartialWritesAndRecover) {
    auto used = panelLedPopulation();
    used[24] = true;
    Pca9956bController driver{2, 32, used};
    LedBus bus;
    PanelLedFrame frame;
    frame.blank = false;
    frame.levels[0] = frame.levels[24] = 70;
    uint32_t now = 0;
    for (; now < 50; ++now)
        driver.service(bus, frame, now);
    ASSERT_FALSE(bus.blanked);
    frame.levels[0] = frame.levels[24] = 80;
    driver.service(bus, frame, now++);  // Freeze snapshot.
    driver.service(bus, frame, now++);  // First bank.
    frame.levels[0] = frame.levels[24] = 90;
    driver.service(bus, frame, now++);  // Second bank still belongs to the frozen frame.
    EXPECT_EQ(bus.registers[0][0x0A], 80);
    EXPECT_EQ(bus.registers[1][0x0A], 80);
    for (unsigned i = 0; i < 50; ++i)
        driver.service(bus, frame, now++);
    frame.levels[0] = frame.levels[24] = 100;
    driver.service(bus, frame, now++);
    driver.service(bus, frame, now++);
    bus.fail_next = true;
    driver.service(bus, frame, now++);
    EXPECT_TRUE(bus.blanked);
    EXPECT_EQ(driver.state(), State::Retry);
    for (unsigned i = 0; i < 1100; ++i)
        driver.service(bus, frame, now++);
    EXPECT_FALSE(bus.blanked);
    EXPECT_EQ(bus.registers[0][0x0A], 100);
    EXPECT_EQ(bus.registers[1][0x0A], 100);
}
}  // namespace
