#include <gtest/gtest.h>

#include "ui_dial.h"
#include "ui_theme.h"
#include "ui_value_tile.h"

#include <cstdint>

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
