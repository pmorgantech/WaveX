#include <gtest/gtest.h>

#include "inter_mcu.h"
#include "midi_task.h"

#include <vector>
using namespace WaveX;
namespace {
std::vector<Protocol::MidiCcMessage> controls;
std::vector<Protocol::MidiPressureMessage> pressures;
}  // namespace
esp_err_t inter_mcu_send_midi_cc(const Protocol::MidiCcMessage& m) {
    controls.push_back(m);
    return ESP_OK;
}
esp_err_t inter_mcu_send_midi_pressure(const Protocol::MidiPressureMessage& m) {
    pressures.push_back(m);
    return ESP_OK;
}
esp_err_t inter_mcu_send_midi_program(const Protocol::MidiProgramMessage&) {
    return ESP_OK;
}
esp_err_t inter_mcu_send_note_on_midi(uint8_t, uint8_t, uint8_t) {
    return ESP_OK;
}
esp_err_t inter_mcu_send_note_off_midi(uint8_t, uint8_t) {
    return ESP_OK;
}
TEST(MidiForward, ParsesAndForwardsCcAndPressureWithOriginalChannel) {
    controls.clear();
    pressures.clear();
    Midi::StreamParser parser;
    Midi::Event event;
    const uint8_t bytes[] = {0xBF, 1, 127, 121, 0, 0xD2, 99, 0xF8, 0};
    for (auto b: bytes)
        if (parser.Feed(b, event))
            midi_forward_event(event);
    ASSERT_EQ(controls.size(), 2u);
    EXPECT_EQ(controls[0].cc, 1);
    EXPECT_EQ(controls[0].value, 127);
    EXPECT_EQ(controls[0].channel, 15);
    EXPECT_EQ(controls[1].cc, 121);
    ASSERT_EQ(pressures.size(), 2u);
    EXPECT_EQ(pressures[0].value, 99);
    EXPECT_EQ(pressures[0].channel, 2);
    EXPECT_EQ(pressures[1].value, 0);
    EXPECT_EQ(pressures[1].channel, 2);
}
