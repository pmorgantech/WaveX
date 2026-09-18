#include "panel/endless_pot.hpp"

#include <gtest/gtest.h>

#include "panel/mcp3208.hpp"
using namespace WaveX::Panel;
namespace {
std::array<uint16_t, 2> Pair(int angle) {
    angle = (angle % kTurn + kTurn) % kTurn;
    auto triangle = [](int phase) {
        phase %= kTurn;
        return static_cast<uint16_t>((phase <= kTurn / 2 ? phase : kTurn - phase) * 4095 /
                                     (kTurn / 2));
    };
    return {triangle(angle), triangle(angle + kTurn / 4)};
}
PotCalibration Enabled(int direction = 1) {
    PotCalibration cal;
    cal.enabled = true;
    cal.direction = static_cast<int8_t>(direction);
    return cal;
}
}  // namespace
TEST(EndlessPot, ReconstructsEveryAngleAcrossBothFoldTransitions) {
    for (int angle = 0; angle < kTurn; ++angle) {
        auto p = Pair(angle);
        uint16_t result = 0;
        ASSERT_TRUE(PotAngle(p[0], p[1], Enabled(), result));
        EXPECT_LE(Magnitude(WrapDelta(int(result) - angle)), 1) << angle;
    }
}
TEST(EndlessPot, CalibratedOffsetAndUnequalWiperSpansPreserveAngle) {
    auto cal = Enabled();
    cal.a = {180, 3810};
    cal.b = {420, 3650};
    for (int angle = 0; angle < kTurn; ++angle) {
        auto raw = Pair(angle);
        raw[0] =
            static_cast<uint16_t>(cal.a.low + int(raw[0]) * (cal.a.high - cal.a.low) / kAdcMax);
        raw[1] =
            static_cast<uint16_t>(cal.b.low + int(raw[1]) * (cal.b.high - cal.b.low) / kAdcMax);
        uint16_t decoded = 0;
        ASSERT_TRUE(PotAngle(raw[0], raw[1], cal, decoded));
        EXPECT_LE(Magnitude(WrapDelta(int(decoded) - angle)), 2) << angle;
    }
}
TEST(EndlessPot, FullTurnsAndReversalsCarryFractionalMovement) {
    for (int increment: {1, 7, 31, 127})
        for (int direction: {1, -1}) {
            EndlessPot pot;
            pot.Configure(Enabled(direction));
            int total = 0;
            uint32_t time = UINT32_MAX - 100;
            for (int angle = 0; angle <= kTurn * 3; angle += increment) {
                auto p = Pair(angle);
                total += pot.Update(p[0], p[1], time++).steps;
            }
            EXPECT_LE(Magnitude(total - direction * 192), 3);
            int reverse = 0;
            for (int angle = kTurn * 3; angle >= 0; angle -= increment) {
                auto p = Pair(angle);
                reverse += pot.Update(p[0], p[1], time++).steps;
            }
            EXPECT_LE(Magnitude(total + reverse), 2);
        }
}
TEST(EndlessPot, IdleJitterDoesNotWalkAndSlowMotionAccumulates) {
    EndlessPot pot;
    pot.Configure(Enabled());
    auto p = Pair(420);
    for (uint32_t i = 0; i < 10000; ++i) {
        auto r = pot.Update(static_cast<uint16_t>(p[0] + int(i % 5) - 2), p[1], i * 2);
        ASSERT_TRUE(r.valid);
        EXPECT_EQ(r.steps, 0);
    }
    int total = 0;
    for (int angle = 420; angle < 420 + 128; ++angle) {
        p = Pair(angle);
        total += pot.Update(p[0], p[1], 20000u + static_cast<uint32_t>(angle - 420) * 2).steps;
    }
    EXPECT_GE(total, 1);
}
TEST(EndlessPot, DisabledFaultedStaleAndDiscontinuousInputNeverJumps) {
    EndlessPot pot;
    auto p = Pair(100);
    EXPECT_FALSE(pot.Update(p[0], p[1], 0).valid);
    pot.Configure(Enabled());
    EXPECT_EQ(pot.Update(p[0], p[1], 0).steps, 0);
    EXPECT_FALSE(pot.Update(0, 0, 2).valid);
    p = Pair(2500);
    EXPECT_EQ(pot.Update(p[0], p[1], 4).steps, 0);
    p = Pair(3000);
    EXPECT_EQ(pot.Update(p[0], p[1], 200).steps, 0);
    p = Pair(1000);
    EXPECT_EQ(pot.Update(p[0], p[1], 202).steps, 0);
    EXPECT_FALSE(pot.Update(4095, 4095, 204).valid);
    EXPECT_FALSE(pot.Update(4096, 2048, 206).valid);
}
TEST(PotCalibrationSession, FrozenRangesNeedAnotherFullClockwiseTurnBeforeSave) {
    for (int direction: {1, -1}) {
        PotCalibrationSession session;
        session.Begin();
        EXPECT_FALSE(session.Verify());
        for (int angle = 0; angle <= kTurn; angle += 16) {
            auto p = Pair(angle);
            session.Update(p[0], p[1]);
        }
        ASSERT_TRUE(session.Verify());
        EXPECT_FALSE(session.Candidate().enabled);
        for (int angle = 0; angle <= kTurn + 64; angle += 16) {
            auto p = Pair(angle * direction);
            session.Update(p[0], p[1]);
        }
        ASSERT_EQ(session.State(), PotCalibrationSession::Stage::Ready);
        EXPECT_TRUE(session.Candidate().enabled);
        EXPECT_EQ(session.Candidate().direction, direction);
        session.Cancel();
        EXPECT_EQ(session.State(), PotCalibrationSession::Stage::Idle);
    }
}
TEST(PotCalibrationSession, MissingAndStuckWipersCannotCompleteCalibration) {
    PotCalibrationSession session;
    session.Begin();
    for (int i = 0; i < 100; ++i)
        session.Update(0, 4095);
    EXPECT_FALSE(session.Verify());
    for (int a = 0; a <= kTurn; a += 16) {
        auto p = Pair(a);
        session.Update(p[0], p[1]);
    }
    ASSERT_TRUE(session.Verify());
    for (int a = 0; a < kTurn / 2; a += 16) {
        auto p = Pair(a);
        session.Update(p[0], p[1]);
    }
    session.Missing();
    EXPECT_EQ(session.Progress(), 0);
    for (int a = kTurn / 2; a <= kTurn; a += 16) {
        auto p = Pair(a);
        session.Update(p[0], p[1]);
    }
    EXPECT_EQ(session.State(), PotCalibrationSession::Stage::Verify);
}
TEST(PotCalibration, VersionedRoundTripRejectsMalformedDataWithoutChangingAuthority) {
    Calibration c;
    c[2] = {{100, 3900}, {40, 4000}, -1, true};
    auto bytes = EncodeCalibration(c);
    Calibration read;
    ASSERT_TRUE(DecodeCalibration(bytes.data(), bytes.size(), read));
    EXPECT_EQ(EncodeCalibration(read), bytes);
    auto before = EncodeCalibration(read);
    for (size_t at: {size_t(0), size_t(1), size_t(10), size_t(11)}) {
        auto bad = bytes;
        bad[at] = 255;
        EXPECT_FALSE(DecodeCalibration(bad.data(), bad.size(), read));
        EXPECT_EQ(EncodeCalibration(read), before);
    }
    EXPECT_FALSE(DecodeCalibration(bytes.data(), bytes.size() - 1, read));
    auto bad = bytes;
    bad[2] = 0;
    bad[3] = 16;  // low beyond ADC full scale
    EXPECT_FALSE(DecodeCalibration(bad.data(), bad.size(), read));
}
TEST(Mcp3208, EveryChannelCommandAndTwelveBitResult) {
    for (uint8_t channel = 0; channel < 8; ++channel) {
        auto cmd = Mcp3208Command(channel);
        EXPECT_EQ(cmd[0], 0);
        EXPECT_EQ(cmd[1] & 6, 6);
        EXPECT_EQ(((cmd[1] & 1) << 2) | (cmd[2] >> 6), channel);
        EXPECT_EQ(cmd[3], 0);
    }
    for (unsigned value = 0; value < 4096; ++value) {
        uint8_t rx[]{
            0xff, 0xff, static_cast<uint8_t>(0xe0 | (value >> 8)), static_cast<uint8_t>(value)};
        uint16_t decoded = 0;
        ASSERT_TRUE(Mcp3208Result(rx, decoded));
        EXPECT_EQ(decoded, value);
    }
    uint8_t rx[]{0xff, 0xff, 0xff, 0xff};
    uint16_t result = 123;
    EXPECT_FALSE(Mcp3208Result(rx, result));
    EXPECT_EQ(result, 123);
}
