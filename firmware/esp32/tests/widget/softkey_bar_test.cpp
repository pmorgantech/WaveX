#include <gtest/gtest.h>

#include "ui/ui_navigator.h"
#include "ui/ui_softkey_bar.h"

#include <cstdint>

// The widget owns scheduling; navigation itself is covered by two-board HIL.
namespace wavex_ui {
void UINavigator::notifySoftkeyUsed() {}
}  // namespace wavex_ui

namespace {
uint16_t framebuffer[1280 * 720];
uint32_t tick_ms = 0;
uint32_t flushes = 0;
uint32_t Tick() {
    return tick_ms;
}
void Flush(lv_display_t* display, const lv_area_t*, uint8_t*) {
    ++flushes;
    lv_display_flush_ready(display);
}
lv_display_t* Display() {
    static lv_display_t* display = nullptr;
    if (!display) {
        lv_init();
        lv_tick_set_cb(Tick);
        display = lv_display_create(1280, 720);
        lv_display_set_buffers(
            display, framebuffer, nullptr, sizeof(framebuffer), LV_DISPLAY_RENDER_MODE_DIRECT);
        lv_display_set_flush_cb(display, Flush);
    }
    return display;
}
class SoftkeyBarRenderTest : public ::testing::Test {
   protected:
    void SetUp() override {
        display_ = Display();
        screen_ = lv_obj_create(nullptr);
        lv_screen_load(screen_);
        bar_.create(screen_);
    }
    void TearDown() override {
        bar_.cancelPending();
        lv_obj_delete(screen_);
    }
    void Draw() {
        tick_ms += 100;
        lv_timer_handler();
        lv_refr_now(display_);
    }
    void Settle() {
        for (int i = 0; i < 4; ++i)
            Draw();
    }
    lv_obj_t* Button(int index) { return lv_obj_get_child(bar_.container(), index); }
    lv_display_t* display_ = nullptr;
    lv_obj_t* screen_ = nullptr;
    wavex_ui::SoftkeyBar bar_;
};
}  // namespace

TEST_F(SoftkeyBarRenderTest, IdenticalAppearanceDoesNotRedrawButReplacesItsAction) {
    int old_calls = 0, new_calls = 0;
    std::array<wavex_ui::Softkey, wavex_ui::NUM_SOFTKEYS> keys{};
    keys[0] = {"Run", [&] { ++old_calls; }};
    bar_.setSoftkeys(keys);
    Settle();
    flushes = 0;
    keys[0].onPress = [&] { ++new_calls; };
    keys[0].why = "Current action";
    for (int i = 0; i < 10; ++i)
        bar_.setSoftkeys(keys);
    Draw();
    EXPECT_EQ(flushes, 0u);
    EXPECT_STREQ(bar_.key(0).why.c_str(), "Current action");
    ASSERT_TRUE(bar_.press(0));
    Draw();
    EXPECT_EQ(old_calls, 0);
    EXPECT_EQ(new_calls, 1);
}

TEST_F(SoftkeyBarRenderTest, EmptyDisabledAndRecreatedSlotsKeepCorrectInteraction) {
    std::array<wavex_ui::Softkey, wavex_ui::NUM_SOFTKEYS> keys{};
    bar_.setSoftkeys(keys);
    EXPECT_FALSE(bar_.press(0));
    EXPECT_FALSE(lv_obj_has_flag(Button(0), LV_OBJ_FLAG_CLICKABLE));
    EXPECT_EQ(lv_obj_get_style_border_width(Button(0), LV_PART_MAIN), 0);
    keys[0] = {"Run", [] {}, false, "Unavailable"};
    bar_.setSoftkeys(keys);
    EXPECT_FALSE(bar_.press(0));
    bar_.create(screen_);
    bar_.setSoftkeys(keys);
    EXPECT_FALSE(lv_obj_has_flag(Button(0), LV_OBJ_FLAG_CLICKABLE));
    keys[0].enabled = true;
    bar_.setSoftkeys(keys);
    EXPECT_TRUE(lv_obj_has_flag(Button(0), LV_OBJ_FLAG_CLICKABLE));
    EXPECT_GT(lv_obj_get_style_border_width(Button(0), LV_PART_MAIN), 0);
    EXPECT_TRUE(bar_.press(0));
    Draw();
}
