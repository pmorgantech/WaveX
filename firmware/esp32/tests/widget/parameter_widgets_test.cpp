#include <gtest/gtest.h>

#include "ui/multi_touch_input.h"
#include "ui_dial.h"
#include "ui_theme.h"
#include "ui_value_tile.h"

#include <array>
#include <cstdint>
#include <utility>

namespace {
uint16_t pixels[1280 * 720];
uint32_t tick = 0, flushes = 0;
uint32_t Tick() {
    return tick;
}
void Flush(lv_display_t* display, const lv_area_t*, uint8_t*) {
    ++flushes;
    lv_display_flush_ready(display);
}
class ParameterWidgetsTest : public ::testing::Test {
   protected:
    void SetUp() override {
        static bool initialized = false;
        if (!initialized) {
            lv_init();
            lv_tick_set_cb(Tick);
            display_ = lv_display_create(1280, 720);
            lv_display_set_buffers(
                display_, pixels, nullptr, sizeof(pixels), LV_DISPLAY_RENDER_MODE_DIRECT);
            lv_display_set_flush_cb(display_, Flush);
            initialized = true;
        }
        display_ = lv_display_get_default();
        screen_ = lv_obj_create(nullptr);
        lv_screen_load(screen_);
        tile_ = wavex_ui::valueTileCreate(screen_, 10, 10, 300, 220, "Value", "%");
        dial_ = wavex_ui::dialCreate(screen_, 340, 10, 500, 240, "Envelope");
    }
    void TearDown() override { lv_obj_delete(screen_); }
    void Draw() {
        tick += 100;
        lv_timer_handler();
        lv_refr_now(display_);
    }
    void Snapshot() {
        wavex_ui::valueTileSetValue(tile_, "50");
        wavex_ui::valueTileSetFill(tile_, .5f);
        wavex_ui::valueTileSetTone(tile_, wavex_ui::TileTone::Positive);
        wavex_ui::valueTileSetFocus(tile_, true);
        wavex_ui::valueTileSetDesc(tile_, "Current level");
        wavex_ui::dialSetValue(dial_, .5f, "500", "ms");
        wavex_ui::dialSetFocus(dial_, true);
    }
    lv_display_t* display_ = nullptr;
    lv_obj_t* screen_ = nullptr;
    wavex_ui::ValueTile tile_;
    wavex_ui::Dial dial_;
};
}  // namespace

TEST_F(ParameterWidgetsTest, UnchangedSnapshotsDoNotInvalidateTheDisplay) {
    Snapshot();
    for (int i = 0; i < 4; ++i)
        Draw();
    flushes = 0;
    for (int i = 0; i < 20; ++i) {
        Snapshot();
        Draw();
    }
    EXPECT_EQ(flushes, 0u);
    wavex_ui::valueTileSetValue(tile_, "51");
    wavex_ui::dialSetValue(dial_, .6f, "600", "ms");
    Draw();
    EXPECT_GT(flushes, 0u);
    EXPECT_STREQ(lv_label_get_text(tile_.value), "51");
    EXPECT_EQ(lv_arc_get_value(dial_.arc), 600);
}

TEST_F(ParameterWidgetsTest, CompactValuesRestoreTheNumericLayout) {
    wavex_ui::valueTileSetValue(tile_, "Long compact name", true);
    Draw();
    EXPECT_EQ(lv_obj_get_style_width(tile_.value, LV_PART_MAIN), tile_.bar_width);
    wavex_ui::valueTileSetValue(tile_, "25");
    Draw();
    EXPECT_EQ(lv_obj_get_style_width(tile_.value, LV_PART_MAIN), LV_SIZE_CONTENT);
    EXPECT_EQ(lv_obj_get_style_text_font(tile_.value, LV_PART_MAIN), tile_.value_font);
    EXPECT_STREQ(lv_label_get_text(tile_.value), "25");
}

TEST_F(ParameterWidgetsTest, FocusToneAndCopiedHandlesPreserveCurrentAppearance) {
    Snapshot();
    auto copy = tile_;
    wavex_ui::valueTileSetFocus(copy, false);
    EXPECT_TRUE(
        lv_color_eq(lv_obj_get_style_bg_color(tile_.bar_fill, LV_PART_MAIN), UI_COLOR_POSITIVE));
    wavex_ui::valueTileSetTone(tile_, wavex_ui::TileTone::Neutral);
    EXPECT_TRUE(lv_color_eq(lv_obj_get_style_bg_color(tile_.bar_fill, LV_PART_MAIN), UI_COLOR_FG));
    wavex_ui::valueTileSetFocus(tile_, true);
    EXPECT_TRUE(
        lv_color_eq(lv_obj_get_style_bg_color(tile_.bar_fill, LV_PART_MAIN), UI_COLOR_ACCENT));
    EXPECT_EQ(lv_obj_get_style_border_width(tile_.card, LV_PART_MAIN), UI_BORDER_WIDTH_FOCUS);
    wavex_ui::dialSetFocus(dial_, false);
    EXPECT_TRUE(lv_color_eq(lv_obj_get_style_arc_color(dial_.arc, LV_PART_INDICATOR), UI_COLOR_FG));
}

TEST_F(ParameterWidgetsTest, TwoParametersKeepTheirFingerAcrossReorderReleaseAndReplacement) {
    int tile_steps = 0, dial_steps = 0;
    wavex_ui::valueTileSetOnAdjust(tile_, [&](int delta) { tile_steps += delta; });
    wavex_ui::dialSetOnAdjust(dial_, [&](int delta) { dial_steps += delta; });
    Draw();
    wavex_ui::MultiTouchInput touch;
    ASSERT_TRUE(touch.init(display_));
    wavex_ui::TouchContact points[] = {{7, {100, 100}}, {42, {440, 100}}};
    touch.update(points, 2);
    points[0].point.y -= 18;
    points[1].point.y -= 27;
    touch.update(points, 2);
    EXPECT_EQ(tile_steps, 2);
    EXPECT_EQ(dial_steps, 3);
    std::swap(points[0], points[1]);
    points[0].point.y -= 9;
    points[1].point.y -= 9;
    touch.update(points, 2);
    EXPECT_EQ(tile_steps, 3);
    EXPECT_EQ(dial_steps, 4);
    // Release the first-created pointer; the second must remain on its dial.
    touch.update(points, 1);
    points[0].point.y -= 9;
    touch.update(points, 1);
    EXPECT_EQ(tile_steps, 3);
    EXPECT_EQ(dial_steps, 5);
    points[1] = {99, {100, 100}};
    touch.update(points, 2);
    points[1].point.y -= 18;
    points[0].point.y -= 9;
    touch.update(points, 2);
    EXPECT_EQ(tile_steps, 5);
    EXPECT_EQ(dial_steps, 6);
    // Release in the opposite order.
    touch.update(&points[1], 1);
    points[1].point.y -= 9;
    touch.update(&points[1], 1);
    EXPECT_EQ(tile_steps, 6);
    EXPECT_EQ(dial_steps, 6);
    touch.update(nullptr, 0);
    touch.deinit();
}

TEST_F(ParameterWidgetsTest, FiveContactsReleaseTheirOwnButtons) {
    lv_obj_clean(screen_);
    std::array<int, 5> presses{}, releases{};
    std::array<wavex_ui::TouchContact, 5> points{};
    for (size_t i = 0; i < points.size(); ++i) {
        auto* button = lv_button_create(screen_);
        lv_obj_set_pos(button, static_cast<int32_t>(i) * 200, 0);
        lv_obj_set_size(button, 180, 180);
        lv_obj_add_event_cb(
            button,
            [](lv_event_t* e) { ++*static_cast<int*>(lv_event_get_user_data(e)); },
            LV_EVENT_PRESSED,
            &presses[i]);
        lv_obj_add_event_cb(
            button,
            [](lv_event_t* e) { ++*static_cast<int*>(lv_event_get_user_data(e)); },
            LV_EVENT_RELEASED,
            &releases[i]);
        points[i] = {static_cast<uint8_t>(i + 10), {static_cast<int32_t>(i) * 200 + 80, 80}};
    }
    Draw();
    wavex_ui::MultiTouchInput touch;
    ASSERT_TRUE(touch.init(display_));
    touch.update(points.data(), points.size());
    for (int press: presses)
        EXPECT_EQ(press, 1);
    touch.update(points.data() + 1, points.size() - 1);
    EXPECT_EQ(releases[0], 1);
    for (size_t i = 1; i < points.size(); ++i)
        EXPECT_EQ(releases[i], 0);
    touch.update(nullptr, 0);
    for (int release: releases)
        EXPECT_EQ(release, 1);
    touch.deinit();
}

TEST_F(ParameterWidgetsTest, ReplacingAnIdDeliversReleaseBeforeNewPress) {
    Draw();
    int presses = 0, releases = 0;
    lv_obj_add_event_cb(
        tile_.card,
        [](lv_event_t* e) { ++*static_cast<int*>(lv_event_get_user_data(e)); },
        LV_EVENT_PRESSED,
        &presses);
    lv_obj_add_event_cb(
        tile_.card,
        [](lv_event_t* e) { ++*static_cast<int*>(lv_event_get_user_data(e)); },
        LV_EVENT_RELEASED,
        &releases);
    wavex_ui::MultiTouchInput touch;
    ASSERT_TRUE(touch.init(display_));
    wavex_ui::TouchContact point{1, {100, 100}};
    touch.update(&point, 1);
    point.id = 2;
    touch.update(&point, 1);
    EXPECT_EQ(presses, 2);
    EXPECT_EQ(releases, 1);
    touch.update(nullptr, 0);
    EXPECT_EQ(releases, 2);
    touch.deinit();
}

TEST_F(ParameterWidgetsTest, DeletingHeldWidgetsDoesNotLeaveDanglingPointers) {
    Draw();
    wavex_ui::MultiTouchInput touch;
    ASSERT_TRUE(touch.init(display_));
    wavex_ui::TouchContact points[] = {{1, {100, 100}}, {2, {440, 100}}};
    touch.update(points, 2);
    lv_obj_clean(screen_);
    touch.update(points, 2);
    touch.update(nullptr, 0);
    touch.deinit();
}
