#include <gtest/gtest.h>

#include "ui/ui_busy_overlay.h"

#include <cstring>
namespace {
uint16_t pixels[1280 * 800];
uint32_t tick = 0;
uint32_t Tick() {
    return tick;
}
void Flush(lv_display_t* display, const lv_area_t*, uint8_t*) {
    lv_display_flush_ready(display);
}
lv_obj_t* FindText(lv_obj_t* object, const char* text) {
    if (lv_obj_check_type(object, &lv_label_class) && !strcmp(lv_label_get_text(object), text))
        return object;
    for (uint32_t i = 0; i < lv_obj_get_child_count(object); ++i)
        if (auto* found = FindText(lv_obj_get_child(object, i), text))
            return found;
    return nullptr;
}
unsigned VisibleTypeCount(lv_obj_t* object, const lv_obj_class_t* type) {
    unsigned count = lv_obj_check_type(object, type) && lv_obj_is_visible(object) ? 1u : 0u;
    for (uint32_t i = 0; i < lv_obj_get_child_count(object); ++i)
        count += VisibleTypeCount(lv_obj_get_child(object, i), type);
    return count;
}
class BusyOverlayTest : public ::testing::Test {
   protected:
    void SetUp() override {
        static bool initialized = false;
        if (!initialized) {
            lv_init();
            lv_tick_set_cb(Tick);
            auto* display = lv_display_create(1280, 800);
            lv_display_set_buffers(
                display, pixels, nullptr, sizeof(pixels), LV_DISPLAY_RENDER_MODE_DIRECT);
            lv_display_set_flush_cb(display, Flush);
            initialized = true;
        }
    }
    void TearDown() override { wavex_ui::BusyOverlay::hide(); }
};
}  // namespace
TEST_F(BusyOverlayTest, FailureRetainsTextAndRejectsLateProgressAndHideUntilDismissed) {
    using namespace wavex_ui::BusyOverlay;
    showDual("Loading", "piano", 100);
    failure("Load failed", "Card removed");
    requestDualProgress(90, 90, "late.wav");
    service();
    requestHide();
    service();
    tick += 1000;
    lv_timer_handler();
    EXPECT_TRUE(isVisible());
    EXPECT_EQ(VisibleTypeCount(lv_screen_active(), &lv_spinner_class), 0u);
    EXPECT_EQ(VisibleTypeCount(lv_screen_active(), &lv_bar_class), 0u);
    ASSERT_NE(FindText(lv_screen_active(), "Load failed"), nullptr);
    ASSERT_NE(FindText(lv_screen_active(), "Card removed"), nullptr);
    auto* dismiss = FindText(lv_screen_active(), "Dismiss");
    ASSERT_NE(dismiss, nullptr);
    lv_obj_send_event(lv_obj_get_parent(dismiss), LV_EVENT_CLICKED, nullptr);
    EXPECT_FALSE(isVisible());
}
TEST_F(BusyOverlayTest, TimeoutStopsWorkAndNewOperationCanComplete) {
    using namespace wavex_ui::BusyOverlay;
    show("Loading", "sample", 100);
    tick += 101;
    lv_timer_handler();
    ASSERT_NE(FindText(lv_screen_active(), "No response from backend"), nullptr);
    EXPECT_TRUE(isVisible());
    EXPECT_EQ(VisibleTypeCount(lv_screen_active(), &lv_spinner_class), 0u);
    EXPECT_EQ(VisibleTypeCount(lv_screen_active(), &lv_bar_class), 0u);
    show("Next load", "sample", 1000);
    requestHide();
    service();
    EXPECT_FALSE(isVisible());
}
