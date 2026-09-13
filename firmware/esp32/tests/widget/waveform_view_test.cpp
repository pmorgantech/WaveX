// Pixel-level tests for WaveformView, rendered with the REAL vendored LVGL.
//
// These exist because the stacked L/R change (roadmap 1.5.7 item 1) landed
// with no automated coverage, justified at the time by "there is no LVGL on
// the host". That was wrong - see docs/roadmap.md. LVGL's software renderer
// draws into a plain memory framebuffer with no display driver, no SDL and no
// hardware, which is exactly what tools/ui_preview has been doing all along.
//
// What this pins is the one thing reading the code could not settle: that L
// and R land in the lanes their labels name. A stereo envelope is
// channel-interleaved (`columns[column * channels + ch]`), so a stride error
// swaps the channels silently - both lanes still draw a plausible waveform,
// and only a hard-panned fixture tells them apart.
//
// Deliberately NOT asserted: exact colours, label glyphs and pixel-exact
// geometry. Those are appearance, they change whenever the design does, and a
// test that fails on a palette tweak trains people to ignore it. Legibility at
// half height is still a bench question.

#include "waveform_view.h"

#include <gtest/gtest.h>

#include "lvgl.h"

#include <algorithm>
#include <cstdint>
#include <vector>

using WaveX::Protocol::EnvelopeColumn;
using wavex_ui::WaveformView;

namespace {

// Small enough to render fast, large enough that a lane still has room for a
// trace to be distinguishable from its zero line.
constexpr int32_t kDisplayW = 256;
constexpr int32_t kDisplayH = 200;

uint16_t g_framebuffer[kDisplayW * kDisplayH];
uint32_t g_tick_ms = 0;

uint32_t TickCb() {
    return g_tick_ms;
}

// LVGL insists on a flush callback. Rendering is DIRECT, so the pixels are
// already in g_framebuffer by the time this runs and there is nothing to copy.
void FlushCb(lv_display_t* disp, const lv_area_t*, uint8_t*) {
    lv_display_flush_ready(disp);
}

lv_display_t* EnsureDisplay() {
    static lv_display_t* disp = nullptr;
    if (!disp) {
        lv_init();
        lv_tick_set_cb(&TickCb);
        disp = lv_display_create(kDisplayW, kDisplayH);
        lv_display_set_buffers(
            disp, g_framebuffer, nullptr, sizeof(g_framebuffer), LV_DISPLAY_RENDER_MODE_DIRECT);
        lv_display_set_flush_cb(disp, &FlushCb);
    }
    return disp;
}

/// A stereo envelope whose two channels have deliberately different levels,
/// which is what makes a channel swap visible at all.
std::vector<EnvelopeColumn> StereoEnvelope(int16_t left_amp, int16_t right_amp, uint16_t columns) {
    std::vector<EnvelopeColumn> v;
    v.reserve(static_cast<size_t>(columns) * 2);
    for (uint16_t i = 0; i < columns; ++i) {
        v.emplace_back(static_cast<int16_t>(-left_amp), left_amp);
        v.emplace_back(static_cast<int16_t>(-right_amp), right_amp);
    }
    return v;
}

std::vector<EnvelopeColumn> MonoEnvelope(int16_t amp, uint16_t columns) {
    std::vector<EnvelopeColumn> v;
    v.reserve(columns);
    for (uint16_t i = 0; i < columns; ++i) {
        v.emplace_back(static_cast<int16_t>(-amp), amp);
    }
    return v;
}

class WaveformViewRenderTest : public ::testing::Test {
   protected:
    void SetUp() override {
        display_ = EnsureDisplay();
        screen_ = lv_obj_create(nullptr);
        lv_obj_set_style_bg_color(screen_, lv_color_hex(0x000000), LV_PART_MAIN);
        lv_obj_set_style_pad_all(screen_, 0, LV_PART_MAIN);
        lv_obj_set_style_border_width(screen_, 0, LV_PART_MAIN);
        lv_screen_load(screen_);
    }

    void TearDown() override {
        view_.reset();
        lv_obj_delete(screen_);
        screen_ = nullptr;
    }

    /// Builds the widget filling the display, feeds it @p columns, and draws.
    void Render(const std::vector<EnvelopeColumn>& columns, uint16_t count, uint8_t channels) {
        view_ = std::make_unique<WaveformView>(screen_, kDisplayW, kDisplayH);
        lv_obj_set_pos(view_->root(), 0, 0);
        view_->setEnvelope(columns.data(), count, channels);

        g_tick_ms += 50;
        lv_timer_handler();
        lv_refr_now(display_);
    }

    /// Builds and draws the widget with no envelope fed to it.
    void RenderEmpty() {
        view_ = std::make_unique<WaveformView>(screen_, kDisplayW, kDisplayH);
        lv_obj_set_pos(view_->root(), 0, 0);
        view_->clear();

        g_tick_ms += 50;
        lv_timer_handler();
        lv_refr_now(display_);
    }

    /// The widget's drawable area, queried rather than assumed - padding and
    /// borders are style-driven and would silently shift a hard-coded rect.
    lv_area_t content() const {
        lv_area_t a;
        lv_obj_get_content_coords(view_->root(), &a);
        return a;
    }

    static uint16_t pixel(int32_t x, int32_t y) { return g_framebuffer[y * kDisplayW + x]; }

    /// Non-background pixels in rows [y1, y2], across the full width.
    ///
    /// Counts PIXELS rather than rows on purpose. The grid draws a vertical
    /// line through every row of the panel, so "rows containing any ink"
    /// saturates at 100% for a silent channel and a loud one alike - which is
    /// how the first version of these tests managed to fail against correct
    /// code. By pixel, a lane's grid contribution is a fixed few hundred and
    /// identical between lanes, so comparing lanes leaves only the trace.
    static int32_t inkPixels(const lv_area_t& area, int32_t y1, int32_t y2) {
        int32_t ink = 0;
        for (int32_t y = y1; y <= y2; ++y) {
            for (int32_t x = area.x1; x <= area.x2; ++x) {
                if (pixel(x, y) != kBackground) {
                    ++ink;
                }
            }
        }
        return ink;
    }

    /// The screen is painted black in SetUp, so anything else is drawn content.
    /// Asserted in BackgroundIsBlack below rather than assumed - if a theme
    /// change made the backdrop non-black, every test here would silently
    /// measure the whole panel and pass for the wrong reason.
    static constexpr uint16_t kBackground = 0x0000;

    lv_display_t* display_ = nullptr;
    lv_obj_t* screen_ = nullptr;
    std::unique_ptr<WaveformView> view_;
};

// Guards the measurement itself. Every assertion below reads "not background"
// as "drawn content", so a theme change that made the backdrop non-black would
// turn the whole panel into ink and make these tests pass unconditionally.
TEST_F(WaveformViewRenderTest, BackgroundIsBlackSoInkMeansContent) {
    RenderEmpty();

    const lv_area_t a = content();
    const int32_t total = lv_area_get_width(&a) * lv_area_get_height(&a);
    const int32_t ink = inkPixels(a, a.y1, a.y2);

    // Grid and the zero line are drawn; everything else must be backdrop.
    EXPECT_GT(ink, 0);
    EXPECT_LT(ink, total / 4) << "backdrop is not black; every measurement here is meaningless";
}

// The regression that matters, and the reason this file exists. Content hard
// panned to the LEFT must fill the TOP lane and leave the bottom near empty.
// Swap the de-interleave stride and this inverts.
TEST_F(WaveformViewRenderTest, HardPannedLeftFillsTopLaneOnly) {
    Render(StereoEnvelope(/*left_amp=*/30000, /*right_amp=*/0, 256), 256, 2);

    const lv_area_t a = content();
    const int32_t mid = a.y1 + lv_area_get_height(&a) / 2;

    EXPECT_GT(inkPixels(a, a.y1, mid - 1), inkPixels(a, mid + 1, a.y2) * 4)
        << "loud channel did not dominate the lane labelled L";
}

// The mirror image. Together with the test above this pins the mapping in both
// directions: either alone would still pass with the lanes swapped.
TEST_F(WaveformViewRenderTest, HardPannedRightFillsBottomLaneOnly) {
    Render(StereoEnvelope(/*left_amp=*/0, /*right_amp=*/30000, 256), 256, 2);

    const lv_area_t a = content();
    const int32_t mid = a.y1 + lv_area_get_height(&a) / 2;

    EXPECT_GT(inkPixels(a, mid + 1, a.y2), inkPixels(a, a.y1, mid - 1) * 4)
        << "loud channel did not dominate the lane labelled R";
}

// A trace must stay inside its own half. Without this, a lane that over-drew
// into its neighbour would still pass both panning tests above.
TEST_F(WaveformViewRenderTest, StereoTraceStaysWithinItsLane) {
    Render(StereoEnvelope(/*left_amp=*/32000, /*right_amp=*/0, 256), 256, 2);

    const lv_area_t a = content();
    const int32_t mid = a.y1 + lv_area_get_height(&a) / 2;

    // Narrow bands either side of the divider. Both contain the same vertical
    // grid pixels, so the difference between them is trace and nothing else -
    // the left channel is at full deflection and the right is silent.
    const int32_t above = inkPixels(a, mid - 4, mid - 1);
    const int32_t below = inkPixels(a, mid + 1, mid + 4);

    EXPECT_GT(above, below * 2) << "left channel bled across the divider";
}

// Mono keeps the whole panel. A regression that always stacked would halve the
// height of every mono waveform and leave the lower half holding only a line.
TEST_F(WaveformViewRenderTest, MonoUsesFullHeight) {
    Render(MonoEnvelope(/*amp=*/30000, 256), 256, 1);

    const lv_area_t a = content();
    const int32_t mid = a.y1 + lv_area_get_height(&a) / 2;
    const int32_t top = inkPixels(a, a.y1, mid - 1);
    const int32_t bottom = inkPixels(a, mid + 1, a.y2);

    // A single trace is symmetric about the centre line, so the two halves
    // carry comparable ink. Stacked, the lower half would be nearly empty.
    EXPECT_GT(top, 0);
    EXPECT_GT(bottom, 0);
    EXPECT_LT(top, bottom * 3);
    EXPECT_LT(bottom, top * 3);
}

// An empty view still draws its zero line. Dropping that once made "no
// envelope has arrived" indistinguishable from "the panel is broken", which is
// exactly how it got reported.
TEST_F(WaveformViewRenderTest, EmptyViewStillDrawsAZeroLine) {
    RenderEmpty();

    const lv_area_t a = content();
    const int32_t mid = a.y1 + lv_area_get_height(&a) / 2;

    // A horizontal run at the centre, wider than the vertical grid lines that
    // cross the same rows.
    EXPECT_GT(inkPixels(a, mid - 1, mid + 1), lv_area_get_width(&a) / 2);
}

// More channels than there are lanes must not read past the pair being drawn.
// The stored count is clamped while the stride stays as sent, so a 3-channel
// payload shows channels 0 and 1 rather than interleaved nonsense.
TEST_F(WaveformViewRenderTest, ExtraChannelsAreClampedNotInterleaved) {
    constexpr uint16_t kColumns = 128;
    std::vector<EnvelopeColumn> v;
    v.reserve(static_cast<size_t>(kColumns) * 3);
    for (uint16_t i = 0; i < kColumns; ++i) {
        v.emplace_back(-30000, 30000);  // ch0: loud   -> top lane
        v.emplace_back(0, 0);           // ch1: silent -> bottom lane
        v.emplace_back(-30000, 30000);  // ch2: loud   -> must be ignored
    }
    Render(v, kColumns, 3);

    const lv_area_t a = content();
    const int32_t mid = a.y1 + lv_area_get_height(&a) / 2;

    EXPECT_GT(inkPixels(a, a.y1, mid - 1), inkPixels(a, mid + 1, a.y2) * 4)
        << "third channel leaked into a lane, or the stride was clamped with the count";
}

// The device renders in WAVEX_LVGL_DRAW_BUF_HEIGHT-line strips, and the draw
// path clips its column runs to the strip it is handed (fewer draw tasks per
// frame - see drawLane). The tests above render DIRECT, whole screen in one
// pass, so they never see a clip narrower than the widget. This renders the
// same busy stereo trace both ways and demands identical pixels: a run merged
// wrongly across a strip edge, or a column dropped at one, shows up here and
// nowhere else on the host.
namespace {
constexpr int32_t kStripH = 8;  // many strips, and edges that cut through runs
uint16_t g_strip_fb[kDisplayW * kDisplayH];
uint16_t g_strip_buf[kDisplayW * kStripH];

void StripFlushCb(lv_display_t* disp, const lv_area_t* area, uint8_t* px_map) {
    const auto* src = reinterpret_cast<const uint16_t*>(px_map);
    const int32_t w = lv_area_get_width(area);
    for (int32_t y = area->y1; y <= area->y2; ++y) {
        std::copy(src, src + w, &g_strip_fb[y * kDisplayW + area->x1]);
        src += w;
    }
    lv_display_flush_ready(disp);
}

std::vector<EnvelopeColumn> BusyStereoEnvelope(uint16_t columns) {
    std::vector<EnvelopeColumn> v;
    v.reserve(static_cast<size_t>(columns) * 2);
    uint32_t seed = 0x9E3779B9u;
    for (uint16_t i = 0; i < columns; ++i) {
        seed = seed * 1664525u + 1013904223u;
        // Loud, quiet and silent stretches, each with jitter, so runs of
        // equal spans exist but are short and end at arbitrary columns.
        const int phase = (i / 20) % 4;
        const int16_t base = phase == 0 ? 30000 : phase == 1 ? 6000 : phase == 2 ? 0 : 15000;
        const int16_t jitter = static_cast<int16_t>((seed >> 16) % 4000);
        const int16_t left = static_cast<int16_t>(base > jitter ? base - jitter : base);
        const int16_t right = static_cast<int16_t>(left / 2 + (seed & 0x3FF));
        v.emplace_back(static_cast<int16_t>(-left), left);
        v.emplace_back(static_cast<int16_t>(-(right / 3)), right);
    }
    return v;
}
}  // namespace

TEST_F(WaveformViewRenderTest, StripRenderMatchesWholeScreenRender) {
    const auto env = BusyStereoEnvelope(kDisplayW);
    Render(env, kDisplayW, 2);
    std::vector<uint16_t> direct(g_framebuffer, g_framebuffer + kDisplayW * kDisplayH);
    const lv_area_t a = content();
    view_.reset();

    lv_display_t* strip = lv_display_create(kDisplayW, kDisplayH);
    lv_display_set_buffers(
        strip, g_strip_buf, nullptr, sizeof(g_strip_buf), LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(strip, &StripFlushCb);
    lv_display_set_default(strip);
    lv_obj_t* screen = lv_obj_create(nullptr);
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x000000), LV_PART_MAIN);
    lv_obj_set_style_pad_all(screen, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(screen, 0, LV_PART_MAIN);
    lv_screen_load(screen);
    {
        WaveformView view(screen, kDisplayW, kDisplayH);
        lv_obj_set_pos(view.root(), 0, 0);
        view.setEnvelope(env.data(), kDisplayW, 2);
        g_tick_ms += 50;
        lv_timer_handler();
        lv_refr_now(strip);
    }
    lv_display_set_default(display_);

    size_t differing = 0;
    for (int32_t y = a.y1; y <= a.y2; ++y) {
        for (int32_t x = a.x1; x <= a.x2; ++x) {
            if (direct[y * kDisplayW + x] != g_strip_fb[y * kDisplayW + x]) {
                ++differing;
            }
        }
    }
    EXPECT_EQ(differing, 0u) << "strip-clipped draw differs from the whole-screen draw";
    EXPECT_GT(inkPixels(a, a.y1, a.y2), 0) << "nothing drawn at all - the comparison is vacuous";

    lv_obj_delete(screen);
    lv_display_delete(strip);
}

}  // namespace
