/**
 * @file wavex_application_test.cpp
 * @brief Unit tests for WaveXApplication initialization ordering and
 *        failure handling.
 *
 * The subsystem entry points (inter_mcu_init/start, pcnt_task_init/start,
 * wavex_ui_task_start) are mocked in mocks/esp32_mocks.cpp with configurable
 * results recorded in InterMcuCapture, so these tests drive the REAL
 * WaveXApplication::initialize() logic through its failure paths.
 */

#include "wavex_application.h"

#include <gtest/gtest.h>

#include "../mocks/esp32_mocks.h"

namespace {

using WaveX::Test::GetInterMcuCapture;
using WaveX::Test::ResetInterMcuCapture;

class WaveXApplicationTest : public ::testing::Test {
   protected:
    void SetUp() override { ResetInterMcuCapture(); }
};

TEST_F(WaveXApplicationTest, InitializeStartsAllSubsystemsInOrder) {
    WaveX::WaveXApplication app;

    EXPECT_TRUE(app.initialize());

    const auto& cap = GetInterMcuCapture();
    EXPECT_EQ(cap.inter_mcu_init_calls, 1);
    EXPECT_EQ(cap.inter_mcu_start_calls, 1);
    EXPECT_EQ(cap.pcnt_init_calls, 1);
    EXPECT_EQ(cap.pcnt_start_calls, 1);
    EXPECT_EQ(cap.ui_start_calls, 1);
}

TEST_F(WaveXApplicationTest, InitializeIsIdempotent) {
    WaveX::WaveXApplication app;
    ASSERT_TRUE(app.initialize());

    // A second call must succeed without re-initializing any subsystem.
    EXPECT_TRUE(app.initialize());

    const auto& cap = GetInterMcuCapture();
    EXPECT_EQ(cap.inter_mcu_init_calls, 1);
    EXPECT_EQ(cap.pcnt_init_calls, 1);
    EXPECT_EQ(cap.ui_start_calls, 1);
}

TEST_F(WaveXApplicationTest, InterMcuInitFailureAbortsBeforeLaterSubsystems) {
    auto& cap = GetInterMcuCapture();
    cap.inter_mcu_init_result = ESP_FAIL;

    WaveX::WaveXApplication app;
    EXPECT_FALSE(app.initialize());

    EXPECT_EQ(cap.inter_mcu_init_calls, 1);
    EXPECT_EQ(cap.inter_mcu_start_calls, 0) << "start must not run after failed init";
    EXPECT_EQ(cap.pcnt_init_calls, 0);
    EXPECT_EQ(cap.ui_start_calls, 0);
}

TEST_F(WaveXApplicationTest, InterMcuStartFailureAborts) {
    auto& cap = GetInterMcuCapture();
    cap.inter_mcu_start_result = ESP_ERR_TIMEOUT;

    WaveX::WaveXApplication app;
    EXPECT_FALSE(app.initialize());

    EXPECT_EQ(cap.inter_mcu_init_calls, 1);
    EXPECT_EQ(cap.inter_mcu_start_calls, 1);
    EXPECT_EQ(cap.pcnt_init_calls, 0);
    EXPECT_EQ(cap.ui_start_calls, 0);
}

TEST_F(WaveXApplicationTest, PcntFailureAbortsBeforeUi) {
    auto& cap = GetInterMcuCapture();
    cap.pcnt_init_result = ESP_FAIL;

    WaveX::WaveXApplication app;
    EXPECT_FALSE(app.initialize());

    EXPECT_EQ(cap.inter_mcu_init_calls, 1);
    EXPECT_EQ(cap.pcnt_init_calls, 1);
    EXPECT_EQ(cap.pcnt_start_calls, 0);
    EXPECT_EQ(cap.ui_start_calls, 0);
}

TEST_F(WaveXApplicationTest, UiStartFailureFailsInitialization) {
    auto& cap = GetInterMcuCapture();
    cap.ui_start_result = ESP_FAIL;

    WaveX::WaveXApplication app;
    EXPECT_FALSE(app.initialize());
    EXPECT_EQ(cap.ui_start_calls, 1);
}

// A failed initialize() must leave the application retryable: fixing the
// failing subsystem and calling initialize() again performs a full init.
TEST_F(WaveXApplicationTest, FailedInitializeCanBeRetried) {
    auto& cap = GetInterMcuCapture();
    cap.inter_mcu_init_result = ESP_FAIL;

    WaveX::WaveXApplication app;
    ASSERT_FALSE(app.initialize());

    cap.inter_mcu_init_result = ESP_OK;
    EXPECT_TRUE(app.initialize());
    EXPECT_EQ(cap.inter_mcu_init_calls, 2);
    EXPECT_EQ(cap.ui_start_calls, 1);
}

// run() without a successful initialize() must return immediately instead of
// entering the infinite main loop (which would hang the suite if this
// regressed).
TEST_F(WaveXApplicationTest, RunWithoutInitializeReturns) {
    WaveX::WaveXApplication app;
    app.run();
    SUCCEED();  // reaching here at all is the assertion; run() loops forever otherwise
}

}  // namespace
