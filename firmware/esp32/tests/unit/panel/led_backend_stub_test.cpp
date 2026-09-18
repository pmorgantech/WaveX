#include <gtest/gtest.h>

#include "panel/led_backend.h"
TEST(LedBackendStub, ReplacementIsExplicitlyUnavailableAndSafeToShutDown) {
    const auto& backend = wavex_panel::SelectedLedBackend();
    EXPECT_STREQ(backend.name, "PCA9956B-stub");
    EXPECT_EQ(backend.init(), ESP_ERR_NOT_SUPPORTED);
    wavex_ui::PanelLedFrame frame;
    frame.blanked = false;
    frame.test_all = true;
    EXPECT_EQ(backend.write(frame), ESP_ERR_NOT_SUPPORTED);
    backend.shutdown();
    backend.shutdown();
    EXPECT_EQ(backend.init(), ESP_ERR_NOT_SUPPORTED);
}
