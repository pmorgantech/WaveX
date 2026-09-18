#pragma once
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>

namespace WaveX::Panel {
constexpr uint16_t kAdcMax = 4095;
constexpr int kTurn = 4096;
constexpr int kStep = 64;  // 64 logical steps per revolution
struct Range {
    uint16_t low = 0, high = kAdcMax;
    bool Valid() const { return high <= kAdcMax && high > low && high - low >= 1024; }
};
struct PotCalibration {
    Range a, b;
    int8_t direction = 1;
    bool enabled = false;
    bool Valid() const { return a.Valid() && b.Valid() && (direction == 1 || direction == -1); }
};
using Calibration = std::array<PotCalibration, 4>;
inline int WrapDelta(int delta) {
    if (delta > kTurn / 2)
        delta -= kTurn;
    if (delta < -kTurn / 2)
        delta += kTurn;
    return delta;
}
inline int Magnitude(int value) {
    return value < 0 ? -value : value;
}
inline int Normalize(uint16_t raw, Range range) {
    return std::clamp((int(raw) - range.low) * kTurn / (range.high - range.low), 0, kTurn);
}
// Two triangle wipers a quarter turn apart. Use the wiper furthest from its
// fold; the other wiper distinguishes the rising/falling half. Shape checks
// reject rails/ambiguous wiring; this is not a claim of electrical presence.
inline bool PotAngle(uint16_t raw_a, uint16_t raw_b, const PotCalibration& cal, uint16_t& angle) {
    if (!cal.Valid() || raw_a > kAdcMax || raw_b > kAdcMax)
        return false;
    const int a = Normalize(raw_a, cal.a), b = Normalize(raw_b, cal.b);
    const int da = Magnitude(a - kTurn / 2), db = Magnitude(b - kTurn / 2);
    if (Magnitude(da + db - kTurn / 2) > kTurn / 8)
        return false;
    int value;
    if (da <= db)
        value = b >= kTurn / 2 ? a / 2 : kTurn - a / 2;
    else
        value = a >= kTurn / 2 ? 3 * kTurn / 4 - b / 2 : b / 2 - kTurn / 4;
    angle = static_cast<uint16_t>((value + kTurn) % kTurn);
    return true;
}
struct PotReading {
    uint16_t angle = 0;
    int16_t steps = 0;
    bool valid = false;
};
class EndlessPot {
   public:
    void Configure(const PotCalibration& cal) {
        calibration_ = cal;
        Reset();
    }
    void Reset() {
        primed_ = false;
        remainder_ = 0;
    }
    PotReading Update(uint16_t a, uint16_t b, uint32_t now_ms) {
        PotReading out;
        if (!calibration_.enabled || !PotAngle(a, b, calibration_, out.angle)) {
            Reset();
            return out;
        }
        out.valid = true;
        if (!primed_ || static_cast<uint32_t>(now_ms - last_ms_) > 100) {
            anchor_ = out.angle;
            remainder_ = 0;
            primed_ = true;
        } else {
            const int delta = WrapDelta(int(out.angle) - anchor_);
            if (Magnitude(delta) > kTurn / 8) {
                // Implausible movement or a missed sample: rebase without an edit.
                anchor_ = out.angle;
                remainder_ = 0;
            } else if (Magnitude(delta) > 8) {
                // Keep the anchor inside the dead band so slow movement adds up.
                anchor_ = out.angle;
                remainder_ += delta;
                out.steps = static_cast<int16_t>((remainder_ / kStep) * calibration_.direction);
                remainder_ %= kStep;
            }
        }
        last_ms_ = now_ms;
        return out;
    }

   private:
    PotCalibration calibration_;
    uint16_t anchor_ = 0;
    uint32_t last_ms_ = 0;
    int remainder_ = 0;
    bool primed_ = false;
};

// Range capture, then a separate full clockwise verification sweep against
// frozen ranges. Calibration never emits control changes. Direction is learned
// from that sweep; only Ready may be persisted and enabled.
class PotCalibrationSession {
   public:
    enum class Stage : uint8_t { Idle, Range, Verify, Ready };
    void Begin() {
        candidate_ = {};
        candidate_.a = candidate_.b = {kAdcMax, 0};
        stage_ = Stage::Range;
        samples_ = 0;
        Missing();
    }
    void Cancel() { stage_ = Stage::Idle; }
    Stage State() const { return stage_; }
    const PotCalibration& Candidate() const { return candidate_; }
    int Progress() const { return std::min(Magnitude(movement_) * 100 / kTurn, 100); }
    void Missing() {
        primed_ = false;
        movement_ = 0;
        quadrants_ = 0;
    }
    bool Verify() {
        if (stage_ != Stage::Range || samples_ < 64 || !candidate_.Valid())
            return false;
        stage_ = Stage::Verify;
        Missing();
        return true;
    }
    void Update(uint16_t a, uint16_t b) {
        if (a > kAdcMax || b > kAdcMax) {
            Missing();
            return;
        }
        if (stage_ == Stage::Range) {
            candidate_.a.low = std::min(candidate_.a.low, a);
            candidate_.a.high = std::max(candidate_.a.high, a);
            candidate_.b.low = std::min(candidate_.b.low, b);
            candidate_.b.high = std::max(candidate_.b.high, b);
            if (samples_ < 64)
                ++samples_;
        } else if (stage_ == Stage::Verify) {
            uint16_t angle;
            if (!PotAngle(a, b, candidate_, angle)) {
                Missing();
                return;
            }
            quadrants_ |= static_cast<uint8_t>(1u << (angle / (kTurn / 4)));
            if (primed_) {
                const int delta = WrapDelta(int(angle) - last_);
                if (Magnitude(delta) > kTurn / 8) {
                    Missing();
                    return;
                }
                movement_ += delta;
                if (Magnitude(movement_) >= kTurn && quadrants_ == 15) {
                    candidate_.direction = movement_ > 0 ? 1 : -1;
                    candidate_.enabled = true;
                    stage_ = Stage::Ready;
                }
            }
            last_ = angle;
            primed_ = true;
        }
    }

   private:
    PotCalibration candidate_;
    Stage stage_ = Stage::Idle;
    uint16_t last_ = 0;
    int movement_ = 0;
    uint8_t quadrants_ = 0, samples_ = 0;
    bool primed_ = false;
};

// Versioned explicit bytes, never the ABI/padding of a C++ record. NVS owns
// integrity checking; this parser additionally validates every field.
constexpr size_t kCalibrationBytes = 42;
inline std::array<uint8_t, kCalibrationBytes> EncodeCalibration(const Calibration& values) {
    std::array<uint8_t, kCalibrationBytes> out{};
    out[0] = 1;
    size_t at = 2;
    for (const auto& cal: values) {
        for (uint16_t value: {cal.a.low, cal.a.high, cal.b.low, cal.b.high}) {
            out[at++] = static_cast<uint8_t>(value);
            out[at++] = static_cast<uint8_t>(value >> 8);
        }
        out[at++] = cal.direction == -1 ? 1 : 0;
        out[at++] = cal.enabled ? 1 : 0;
    }
    return out;
}
inline bool DecodeCalibration(const uint8_t* bytes, size_t size, Calibration& out) {
    if (!bytes || size != kCalibrationBytes || bytes[0] != 1 || bytes[1] != 0)
        return false;
    Calibration candidate;
    size_t at = 2;
    for (auto& cal: candidate) {
        uint16_t* fields[]{&cal.a.low, &cal.a.high, &cal.b.low, &cal.b.high};
        for (auto field: fields) {
            *field = static_cast<uint16_t>(bytes[at] | (uint16_t(bytes[at + 1]) << 8));
            at += 2;
        }
        if (bytes[at] > 1 || bytes[at + 1] > 1)
            return false;
        cal.direction = bytes[at++] ? -1 : 1;
        cal.enabled = bytes[at++] != 0;
        if (!cal.Valid())
            return false;
    }
    out = candidate;
    return true;
}
}  // namespace WaveX::Panel
