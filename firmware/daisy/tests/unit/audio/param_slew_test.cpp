// Tests for the parameter slew engine (roadmap Phase 2.5 item 4; design:
// docs/features/scenes-and-performance.md §3).
//
// The behaviours that matter are the ones that produce an audible artefact
// when wrong: a ramp that jumps when retargeted mid-flight, a ramp that stops
// one step short of its target, and a full table that drops a request
// silently. Each has a test below.

#include "audio/param_slew.hpp"

#include <gtest/gtest.h>

#include <cmath>
#include <vector>

using WaveX::AudioEngine::kMaxParamRamps;
using WaveX::AudioEngine::ParamSlewEngine;

namespace {

struct Applied {
    uint8_t param_id;
    uint8_t slot;
    float value;
};

/// Collects what the engine applied, standing in for the live-CC apply path
/// the design requires ramps to share.
struct Recorder {
    std::vector<Applied> calls;
    void operator()(uint8_t param_id, uint8_t slot, float value) {
        calls.push_back({param_id, slot, value});
    }
    void clear() { calls.clear(); }
    float last() const { return calls.empty() ? 0.0f : calls.back().value; }
};

constexpr float kTickHz = 1000.0f;

ParamSlewEngine MakeEngine() {
    ParamSlewEngine e;
    e.Init(kTickHz);
    e.Reset();
    return e;
}

TEST(ParamSlew, ZeroMorphAppliesImmediatelyAndFreesTheSlot) {
    auto engine = MakeEngine();
    Recorder rec;

    ASSERT_TRUE(engine.Start(1, 0, 0.0f, 1.0f, 0));
    engine.Tick(rec);

    ASSERT_EQ(rec.calls.size(), 1u);
    EXPECT_EQ(rec.calls[0].value, 1.0f);
    EXPECT_EQ(engine.ActiveCount(), 0) << "an immediate apply should not hold a slot";
}

TEST(ParamSlew, RampReachesTheTargetExactlyAndStops) {
    auto engine = MakeEngine();
    Recorder rec;
    ASSERT_TRUE(engine.Start(2, 3, 0.0f, 1.0f, 10));  // 10 ms at 1 kHz = 10 ticks

    for (int i = 0; i < 10; ++i) {
        engine.Tick(rec);
    }
    EXPECT_FLOAT_EQ(rec.last(), 1.0f) << "ramp stopped short of its target";
    EXPECT_EQ(engine.ActiveCount(), 0);

    // A further tick must apply nothing at all.
    rec.clear();
    engine.Tick(rec);
    EXPECT_TRUE(rec.calls.empty());
}

TEST(ParamSlew, RampIsMonotonicAndBounded) {
    auto engine = MakeEngine();
    Recorder rec;
    ASSERT_TRUE(engine.Start(4, 0, 0.0f, 1.0f, 50));

    for (int i = 0; i < 60; ++i) {
        engine.Tick(rec);
    }
    ASSERT_FALSE(rec.calls.empty());
    float previous = -1.0f;
    for (const auto& c: rec.calls) {
        EXPECT_GE(c.value, previous) << "ramp went backwards";
        EXPECT_LE(c.value, 1.0f) << "ramp overshot its target";
        previous = c.value;
    }
}

TEST(ParamSlew, DownwardRampWorksTheSameWay) {
    auto engine = MakeEngine();
    Recorder rec;
    ASSERT_TRUE(engine.Start(5, 0, 1.0f, 0.0f, 10));
    for (int i = 0; i < 10; ++i) {
        engine.Tick(rec);
    }
    EXPECT_FLOAT_EQ(rec.last(), 0.0f);
    for (const auto& c: rec.calls) {
        EXPECT_GE(c.value, 0.0f) << "downward ramp undershot";
    }
}

// The collision rule, and the reason this class exists. Retargeting mid-ramp
// must continue from where the value actually is - taking the caller's idea of
// the start point instead is exactly the jump a slew engine is meant to
// remove.
TEST(ParamSlew, RetargetingMidRampContinuesFromTheCurrentValue) {
    auto engine = MakeEngine();
    Recorder rec;
    ASSERT_TRUE(engine.Start(6, 0, 0.0f, 1.0f, 100));
    for (int i = 0; i < 20; ++i) {
        engine.Tick(rec);
    }
    const float mid = rec.last();
    ASSERT_GT(mid, 0.0f);
    ASSERT_LT(mid, 1.0f);

    // New target, and a deliberately wrong `from` - it must be ignored.
    ASSERT_TRUE(engine.Start(6, 0, /*from=*/0.0f, /*to=*/0.5f, 100));
    EXPECT_EQ(engine.ActiveCount(), 1) << "retarget should replace, not add";

    rec.clear();
    engine.Tick(rec);
    ASSERT_EQ(rec.calls.size(), 1u);
    EXPECT_NEAR(rec.calls[0].value, mid, 0.02f)
        << "retarget jumped back to the caller's `from` instead of continuing";
}

TEST(ParamSlew, RampsAreScopedToParamAndSlot) {
    auto engine = MakeEngine();
    Recorder rec;
    ASSERT_TRUE(engine.Start(7, 0, 0.0f, 1.0f, 10));
    ASSERT_TRUE(engine.Start(7, 1, 0.0f, 1.0f, 10));  // same param, other slot
    ASSERT_TRUE(engine.Start(8, 0, 0.0f, 1.0f, 10));  // other param, same slot
    EXPECT_EQ(engine.ActiveCount(), 3) << "distinct targets were merged";
}

TEST(ParamSlew, CancelStopsApplyingWithoutReachingTheTarget) {
    auto engine = MakeEngine();
    Recorder rec;
    ASSERT_TRUE(engine.Start(9, 0, 0.0f, 1.0f, 100));
    engine.Tick(rec);
    engine.Cancel(9, 0);

    rec.clear();
    engine.Tick(rec);
    EXPECT_TRUE(rec.calls.empty());
    EXPECT_EQ(engine.ActiveCount(), 0);
}

// A dropped request must be reported. Silently ignoring it would leave a
// parameter stuck at its old value with nothing to show why.
TEST(ParamSlew, FullTableRefusesRatherThanDroppingSilently) {
    auto engine = MakeEngine();
    for (uint8_t i = 0; i < kMaxParamRamps; ++i) {
        ASSERT_TRUE(engine.Start(i, 0, 0.0f, 1.0f, 100)) << "at ramp " << int(i);
    }
    EXPECT_EQ(engine.ActiveCount(), kMaxParamRamps);
    EXPECT_FALSE(engine.Start(200, 0, 0.0f, 1.0f, 100)) << "overflow was not reported";

    // But retargeting an existing one still works when full - it replaces
    // rather than needing a free slot.
    EXPECT_TRUE(engine.Start(0, 0, 0.0f, 0.25f, 100));
}

// A morph shorter than one tick cannot be ramped, so it must land on the
// target rather than stepping past it forever.
TEST(ParamSlew, SubTickMorphLandsOnTheTarget) {
    auto engine = MakeEngine();
    Recorder rec;
    ASSERT_TRUE(engine.Start(10, 0, 0.0f, 1.0f, 1));  // exactly one tick
    engine.Tick(rec);
    EXPECT_FLOAT_EQ(rec.last(), 1.0f);
    EXPECT_EQ(engine.ActiveCount(), 0);
}

TEST(ParamSlew, ResetClearsEverything) {
    auto engine = MakeEngine();
    Recorder rec;
    ASSERT_TRUE(engine.Start(11, 0, 0.0f, 1.0f, 100));
    engine.Reset();
    EXPECT_EQ(engine.ActiveCount(), 0);
    engine.Tick(rec);
    EXPECT_TRUE(rec.calls.empty());
}

TEST(ParamSlew, ValueReportsARunningRamp) {
    auto engine = MakeEngine();
    Recorder rec;
    ASSERT_TRUE(engine.Start(12, 4, 0.0f, 1.0f, 100));
    engine.Tick(rec);

    float value = -1.0f;
    ASSERT_TRUE(engine.Value(12, 4, value));
    EXPECT_GT(value, 0.0f);
    EXPECT_FALSE(engine.Value(12, 5, value)) << "reported a ramp on the wrong slot";
}

}  // namespace
