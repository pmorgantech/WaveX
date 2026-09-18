#include "panel/pot_service.h"

#include <gtest/gtest.h>

#include "panel/mcp3208.h"
#include "panel/pot_store.h"
using namespace wavex_panel;
using namespace WaveX::Panel;
namespace {
Calibration persisted;
std::array<uint16_t, 8> raw{};
esp_err_t read_error = ESP_OK, save_error = ESP_OK;
int saves = 0;
uint32_t now = 0;
void scan(int angle) {
    auto triangle = [](int phase) {
        phase = (phase % kTurn + kTurn) % kTurn;
        return static_cast<uint16_t>((phase <= kTurn / 2 ? phase : kTurn - phase) * 4095 /
                                     (kTurn / 2));
    };
    for (size_t i = 0; i < 4; ++i) {
        raw[2 * i] = triangle(angle);
        raw[2 * i + 1] = triangle(angle + kTurn / 4);
    }
    ServicePots(now += 2);
}
class PotServiceTest : public ::testing::Test {
   protected:
    void SetUp() override {
        persisted = {};
        raw = {};
        read_error = save_error = ESP_OK;
        saves = 0;
        now = 0;
        InitPots();
        scan(0);
    }
    void TearDown() override { StopPots(); }
    void ready() {
        ASSERT_TRUE(RequestPot(PotCommand::Begin, 0));
        scan(0);
        for (int angle = 0; angle <= kTurn; angle += 16)
            scan(angle);
        ASSERT_TRUE(RequestPot(PotCommand::Verify, 0));
        scan(0);
        for (int angle = 0; angle <= kTurn + 32; angle += 16)
            scan(angle);
        ASSERT_EQ(ReadPots().stage, PotCalibrationSession::Stage::Ready);
    }
};
}  // namespace
namespace wavex_panel {
esp_err_t InitAdc() {
    return ESP_OK;
}
esp_err_t ReadAdc(std::array<uint16_t, 8>& values) {
    if (read_error == ESP_OK)
        values = raw;
    return read_error;
}
void CloseAdc() {}
esp_err_t LoadPotCalibration(Calibration& values) {
    values = persisted;
    return ESP_OK;
}
esp_err_t SavePotCalibration(const Calibration& values) {
    ++saves;
    EXPECT_TRUE(ReadPots().busy);
    EXPECT_FALSE(RequestPot(PotCommand::Begin, 1));
    if (save_error == ESP_OK)
        persisted = values;
    return save_error;
}
}  // namespace wavex_panel
TEST_F(PotServiceTest, FreshDeviceDoesNotEditAndCalibrationSavesOnlyAfterVerification) {
    for (int angle = 0; angle < 2 * kTurn; angle += 16)
        scan(angle);
    for (auto steps: TakePotSteps())
        EXPECT_EQ(steps, 0);
    ASSERT_TRUE(RequestPot(PotCommand::Save, 0));
    scan(0);
    EXPECT_EQ(saves, 0);
    ready();
    for (auto steps: TakePotSteps())
        EXPECT_EQ(steps, 0);
    save_error = ESP_FAIL;
    ASSERT_TRUE(RequestPot(PotCommand::Save, 0));
    scan(0);
    EXPECT_FALSE(ReadPots().calibration[0].enabled);
    EXPECT_EQ(ReadPots().last_result, ESP_FAIL);
    EXPECT_EQ(ReadPots().stage, PotCalibrationSession::Stage::Ready);
    save_error = ESP_OK;
    ASSERT_TRUE(RequestPot(PotCommand::Save, 0));
    scan(0);
    ASSERT_TRUE(ReadPots().calibration[0].enabled);
    EXPECT_TRUE(persisted[0].enabled);
    for (int angle = 0; angle <= 256; angle += 16)
        scan(angle);
    EXPECT_GT(TakePotSteps()[0], 0);
}
TEST_F(PotServiceTest, CancelWinsOverAnUnprocessedSaveAndErrorsDiscardUnconsumedMotion) {
    ready();
    ASSERT_TRUE(RequestPot(PotCommand::Save, 0));
    CancelPotCalibration();
    scan(0);
    EXPECT_EQ(saves, 0);
    EXPECT_FALSE(ReadPots().calibration[0].enabled);
    persisted[0].enabled = true;
    StopPots();
    InitPots();
    scan(0);
    for (int angle = 0; angle <= 256; angle += 16)
        scan(angle);
    read_error = ESP_FAIL;
    scan(256);
    EXPECT_FALSE(ReadPots().adc_ready);
    EXPECT_EQ(TakePotSteps()[0], 0);
    read_error = ESP_OK;
    now += 1000;
    scan(2000);
    EXPECT_EQ(TakePotSteps()[0], 0);
}
TEST_F(PotServiceTest, DisableFailureKeepsTheExistingAuthorityAndSuccessfulDisablePersists) {
    persisted[0].enabled = true;
    StopPots();
    InitPots();
    scan(0);
    save_error = ESP_FAIL;
    ASSERT_TRUE(RequestPot(PotCommand::Disable, 0));
    scan(0);
    EXPECT_TRUE(ReadPots().calibration[0].enabled);
    save_error = ESP_OK;
    ASSERT_TRUE(RequestPot(PotCommand::Disable, 0));
    scan(0);
    EXPECT_FALSE(ReadPots().calibration[0].enabled);
    EXPECT_FALSE(persisted[0].enabled);
}
