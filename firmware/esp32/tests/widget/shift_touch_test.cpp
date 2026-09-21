#include <gtest/gtest.h>

#include "ui/multi_touch_input.h"
#include "ui/ui_navigator.h"
#include "ui_theme.h"

#include <cstring>
#include <memory>

// Meter telemetry is unrelated to navigation/input, and requires the MCU link.
namespace wavex_ui {
void statusStripCreate(lv_obj_t*) {}
}  // namespace wavex_ui

namespace {
uint16_t pixels[UI_SCREEN_WIDTH * UI_SCREEN_HEIGHT];
uint32_t tick = 0;
uint32_t flushes = 0;
uint32_t Tick() {
    return tick;
}
void Flush(lv_display_t* display, const lv_area_t*, uint8_t*) {
    ++flushes;
    lv_display_flush_ready(display);
}
class ShiftPage : public wavex_ui::UIPage {
   public:
    const char* name() const override { return "Shift test"; }
    const char* contextLine() const override {
        return "Track 1 / A long Instrument name / Edited / Slot 8 / "
               "Save a WXI copy from Pad Map / Additional context beyond the meter boundary";
    }
    void onEnter(lv_obj_t* parent) override { root_ = lv_obj_create(parent); }
    void onExit() override {
        lv_obj_delete(root_);
        root_ = nullptr;
    }
    std::array<wavex_ui::Softkey, wavex_ui::NUM_SOFTKEYS> getSoftkeys() override {
        std::array<wavex_ui::Softkey, wavex_ui::NUM_SOFTKEYS> keys{};
        keys[0] = {"Normal", [this] { ++normal; }};
        return keys;
    }
    std::array<wavex_ui::Softkey, wavex_ui::NUM_SOFTKEYS> getShiftedSoftkeys() override {
        std::array<wavex_ui::Softkey, wavex_ui::NUM_SOFTKEYS> keys{};
        keys[0] = {"Alternate", [this] { ++alternate; }};
        return keys;
    }
    int normal = 0, alternate = 0;
};
lv_obj_t* FindShift(lv_obj_t* parent) {
    if (lv_obj_check_type(parent, &lv_label_class) &&
        std::strcmp(lv_label_get_text(parent), "SHIFT") == 0) {
        return lv_obj_get_parent(parent);
    }
    for (uint32_t i = 0; i < lv_obj_get_child_count(parent); ++i) {
        if (auto* found = FindShift(lv_obj_get_child(parent, static_cast<int32_t>(i)))) {
            return found;
        }
    }
    return nullptr;
}
}  // namespace

TEST(ShiftTouchTest, HeldShiftFiresAlternateAndReleaseDoesNotRelatch) {
    lv_init();
    lv_tick_set_cb(Tick);
    auto* display = lv_display_create(UI_SCREEN_WIDTH, UI_SCREEN_HEIGHT);
    lv_display_set_buffers(display, pixels, nullptr, sizeof(pixels), LV_DISPLAY_RENDER_MODE_DIRECT);
    lv_display_set_flush_cb(display, Flush);
    auto page = std::make_shared<ShiftPage>();
    auto& nav = wavex_ui::UINavigator::instance();
    nav.push(page);
    lv_obj_update_layout(lv_screen_active());
    auto* shift = FindShift(lv_screen_active());
    ASSERT_NE(shift, nullptr);
    auto* header = lv_obj_get_parent(shift);
    auto* context = lv_obj_get_child(header, 1);
    ASSERT_TRUE(lv_obj_check_type(context, &lv_label_class));
    EXPECT_EQ(lv_label_get_long_mode(context), LV_LABEL_LONG_DOT);
    EXPECT_LE(lv_obj_get_x(context) + lv_obj_get_width(context),
              UI_HEADER_STATUS_X - UI_HEADER_GAP);
    EXPECT_EQ(lv_obj_get_height(context), lv_font_get_line_height(UI_FONT_BODY));
    lv_refr_now(display);
    flushes = 0;
    nav.refreshContext();
    lv_refr_now(display);
    EXPECT_EQ(flushes, 0u);  // DOT text must not cause an identical refresh to repaint.

    lv_area_t area;
    lv_obj_get_coords(shift, &area);
    wavex_ui::TouchContact contacts[] = {{10, {(area.x1 + area.x2) / 2, (area.y1 + area.y2) / 2}},
                                         {20, {}}};
    ASSERT_TRUE(nav.softkeyBar()->buttonCenter(0, &contacts[1].point.x, &contacts[1].point.y));
    wavex_ui::MultiTouchInput touch;
    ASSERT_TRUE(touch.init(display));
    touch.update(contacts, 1);
    EXPECT_TRUE(nav.isShifted());
    touch.update(contacts, 2);
    touch.update(contacts, 1);  // Click the softkey while Shift remains held.
    tick += 100;
    lv_timer_handler();
    EXPECT_EQ(page->alternate, 1);
    EXPECT_EQ(page->normal, 0);
    EXPECT_FALSE(nav.isShifted());
    touch.update(nullptr, 0);
    EXPECT_FALSE(nav.isShifted());

    // The existing single-finger tap-to-latch and tap-to-clear still work.
    touch.update(contacts, 1);
    touch.update(nullptr, 0);
    EXPECT_TRUE(nav.isShifted());
    touch.update(contacts, 1);
    touch.update(nullptr, 0);
    EXPECT_FALSE(nav.isShifted());

    // Navigation consumes the latch even when the Shift contact stays down.
    touch.update(contacts, 1);
    EXPECT_TRUE(nav.isShifted());
    auto next = std::make_shared<ShiftPage>();
    nav.push(next);
    EXPECT_FALSE(nav.isShifted());
    touch.update(nullptr, 0);
    EXPECT_FALSE(nav.isShifted());
    nav.pop();
    touch.deinit();
}
