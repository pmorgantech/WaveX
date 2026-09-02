// Pins the global encoder direction contract (roadmap 1.5.2 item 5).
//
// Clockwise increases, always. The contract exists because it was broken twice
// in the same way: the rotary encoder posted a SIGNED count while the pot
// posted a magnitude, so a page negating `delta` on an EncoderLeft flipped it
// back to positive. The sample edit page shipped inverted for that reason and
// patched it locally; the voice page still had it afterwards.
//
// These tests are cheap because InputEvent is a header-only POD with no LVGL
// or IDF dependency - which is also why nothing stopped the defect before.

#include "ui/input_event.h"

#include <gtest/gtest.h>

#include <cstdint>

using wavex_ui::InputEvent;
using wavex_ui::InputType;

namespace {

InputEvent Encoder(InputType type, int16_t delta) {
    InputEvent evt;
    evt.type = type;
    evt.delta = delta;
    return evt;
}

TEST(InputEventSteps, ClockwiseIsPositive) {
    EXPECT_EQ(Encoder(InputType::EncoderRight, 3).steps(), 3);
    EXPECT_EQ(Encoder(InputType::EncoderUp, 3).steps(), 3);
}

TEST(InputEventSteps, CounterClockwiseIsNegative) {
    EXPECT_EQ(Encoder(InputType::EncoderLeft, 3).steps(), -3);
    EXPECT_EQ(Encoder(InputType::EncoderDown, 3).steps(), -3);
}

// The regression that matters. A producer posting a signed count - which is
// what ui_task.cpp did for the rotary encoder - must not be able to reverse
// the direction the event type declares.
TEST(InputEventSteps, SignedDeltaCannotInvertDirection) {
    EXPECT_EQ(Encoder(InputType::EncoderLeft, -3).steps(), -3);
    EXPECT_EQ(Encoder(InputType::EncoderDown, -3).steps(), -3);
    EXPECT_EQ(Encoder(InputType::EncoderRight, -3).steps(), 3);
    EXPECT_EQ(Encoder(InputType::EncoderUp, -3).steps(), 3);
}

// A producer that sets the type but forgets the magnitude should still move
// the control, rather than emitting an event that does nothing.
TEST(InputEventSteps, ZeroDeltaStillMovesOneStep) {
    EXPECT_EQ(Encoder(InputType::EncoderRight, 0).steps(), 1);
    EXPECT_EQ(Encoder(InputType::EncoderLeft, 0).steps(), -1);
}

TEST(InputEventSteps, NonEncoderEventsHaveNoSteps) {
    EXPECT_EQ(Encoder(InputType::EncoderClick, 5).steps(), 0);
    EXPECT_EQ(Encoder(InputType::ButtonPress, 5).steps(), 0);
    EXPECT_EQ(Encoder(InputType::TouchDown, 5).steps(), 0);
    EXPECT_EQ(Encoder(InputType::TouchMove, 5).steps(), 0);
}

// int16_t's most negative value has no positive counterpart in int16_t, so the
// magnitude is taken in int. Left stays left; nothing overflows.
TEST(InputEventSteps, MostNegativeDeltaDoesNotOverflow) {
    EXPECT_EQ(Encoder(InputType::EncoderLeft, INT16_MIN).steps(), -32768);
    EXPECT_EQ(Encoder(InputType::EncoderRight, INT16_MIN).steps(), 32768);
}

}  // namespace
