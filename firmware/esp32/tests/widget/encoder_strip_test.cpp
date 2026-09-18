#include "encoder_strip.h"

#include <gtest/gtest.h>

#include <cstdio>
#include <cstring>
namespace {
uint16_t framebuffer[1280 * 720];
uint32_t tick = 0, flushes = 0;
uint32_t Tick() {
    return tick;
}
void Flush(lv_display_t* display, const lv_area_t*, uint8_t*) {
    ++flushes;
    lv_display_flush_ready(display);
}
}  // namespace
TEST(EncoderStrip, IdenticalStateDoesNotRedrawAndReentryBuildsFreshWidgets) {
    lv_init();
    lv_tick_set_cb(Tick);
    auto* display = lv_display_create(1280, 720);
    lv_display_set_buffers(
        display, framebuffer, nullptr, sizeof(framebuffer), LV_DISPLAY_RENDER_MODE_DIRECT);
    lv_display_set_flush_cb(display, Flush);
    auto* screen = lv_obj_create(nullptr);
    lv_screen_load(screen);
    wavex_ui::EncoderStrip strip;
    strip.Create(screen, 497);
    wavex_ui::EncoderBindings bindings;
    bindings[0].label = "CUTOFF";
    bindings[0].enabled = true;
    std::snprintf(bindings[0].value.data(), bindings[0].value.size(), "20000 Hz");
    strip.Update(bindings);
    for (int i = 0; i < 5; ++i) {
        tick += 100;
        lv_timer_handler();
        lv_refr_now(display);
    }
    flushes = 0;
    for (int i = 0; i < 20; ++i)
        strip.Update(bindings);
    tick += 100;
    lv_timer_handler();
    lv_refr_now(display);
    EXPECT_EQ(flushes, 0u);
    std::snprintf(bindings[0].value.data(), bindings[0].value.size(), "1000 Hz");
    strip.Update(bindings);
    lv_refr_now(display);
    EXPECT_GT(flushes, 0u);
    strip.Reset();
    lv_obj_clean(screen);
    strip.Create(screen, 497);
    strip.Update(bindings);
    auto* card = lv_obj_get_child(screen, 0);
    EXPECT_STREQ(lv_label_get_text(lv_obj_get_child(card, 1)), "1000 Hz");
    lv_obj_update_layout(screen);
    for (uint32_t i = 0; i < lv_obj_get_child_count(screen); ++i) {
        auto* obj = lv_obj_get_child(screen, static_cast<int32_t>(i));
        EXPECT_LE(lv_obj_get_y(obj) + lv_obj_get_height(obj), 557);
    }
    strip.Reset();
    lv_obj_delete(screen);
    lv_display_delete(display);
}
