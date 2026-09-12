#include "ui/parameter_lock_model.h"

#include <gtest/gtest.h>

using namespace wavex_ui;
using namespace WaveX::Protocol;
TEST(ParameterLockModel, EmptySlotsCanBeAssignedAndDuplicateParametersAreSkipped) {
    SeqStepState step;
    EXPECT_TRUE(ParameterLocks::Replace(step, 0, PARAM_FILTER_CUTOFF, 123));
    EXPECT_FALSE(ParameterLocks::Replace(step, 1, PARAM_FILTER_CUTOFF, 456));
    EXPECT_EQ(ParameterLocks::Next(step, 1, 1), PARAM_FILTER_RESONANCE);
    EXPECT_TRUE(ParameterLocks::Replace(step, 0, PARAM_PAN, 65535));
    EXPECT_EQ(ParameterLocks::Next(step, 1, 1), PARAM_FILTER_CUTOFF);
    EXPECT_EQ(step.locks[0].value, 65535);
    EXPECT_FALSE(ParameterLocks::Replace(step, 4, PARAM_PAN, 0));
    EXPECT_FALSE(ParameterLocks::Replace(step, 2, PARAM_VOLUME, 0));
}
TEST(ParameterLockModel, ClearAndReplacementPreserveOtherSlots) {
    SeqStepState step;
    ParameterLocks::Replace(step, 0, PARAM_FILTER_CUTOFF, 123);
    ParameterLocks::Replace(step, 3, PARAM_GAIN, 32768);
    ParameterLocks::Replace(step, 0, 0, 456);
    EXPECT_EQ(step.locks[0].parameter, 0);
    EXPECT_EQ(step.locks[0].value, 0);
    EXPECT_EQ(step.locks[3].parameter, PARAM_GAIN);
    EXPECT_EQ(step.locks[3].value, 32768);
}
