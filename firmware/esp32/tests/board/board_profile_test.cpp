#include <gtest/gtest.h>

#include "config/pin_config.h"

TEST(BoardProfile, ProtectsUsbAndBootStrappingPins) {
    for (int pin: {24, 25, 26, 27, 34, 35, 36, 37, 38})
        EXPECT_FALSE(wavex_pins::available(pin)) << pin;
}
TEST(BoardProfile, EveryReservationIsAvailableAndUnique) {
    EXPECT_TRUE(wavex_pins::validAllocation());
}
#if WAVEX_ESP_BOARD == WAVEX_ESP_BOARD_CORE
TEST(BoardProfile, CoreUsesBottomPadAdcChipSelectWithoutBootStrap) {
    EXPECT_EQ(WAVEX_ESP_MCP3208_CS, 31);
    EXPECT_EQ(WAVEX_ESP_BTN_INT, 28);
    EXPECT_GE(WAVEX_ESP_MUX_S0, 0);
    EXPECT_STREQ(WAVEX_ESP_BOARD_NAME, "ESP32-P4-Core-DEV-KIT");
}
#else
TEST(BoardProfile, BenchWiringIsPreserved) {
    EXPECT_EQ(WAVEX_ESP_MCP3208_CS, 5);
    EXPECT_EQ(WAVEX_ESP_BTN_INT, 30);
    EXPECT_EQ(WAVEX_ESP_MUX_S0, -1);
    EXPECT_STREQ(WAVEX_ESP_BOARD_NAME, "ESP32-P4-WIFI6");
}
#endif
