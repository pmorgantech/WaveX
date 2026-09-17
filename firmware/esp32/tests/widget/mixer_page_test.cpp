#include <gtest/gtest.h>

#include "inter_mcu.h"
#include "ui/current_track.h"
#include "ui/mixer_solo.h"
#include "ui/ui_mixer_page.h"
#include "ui/ui_navigator.h"
#include "ui_theme.h"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace wavex_ui {
void statusStripCreate(lv_obj_t*) {}
}  // namespace wavex_ui
namespace {
uint16_t pixels[1280 * 720];
uint32_t tick = 0, flushes = 0;
bool alive = true, respond = true;
WaveX::Protocol::MixStateRequest request;
std::array<WaveX::Protocol::MixStateMessage, 17> targets;
uint16_t solo_mask = 0;
bool meters_fresh = false;
unsigned subscriptions = 0, unsubscriptions = 0;
WaveX::Protocol::MixMetersMessage meters;
uint32_t Tick() {
    return tick;
}
void Flush(lv_display_t* d, const lv_area_t*, uint8_t*) {
    ++flushes;
    lv_display_flush_ready(d);
}
unsigned Index(uint8_t track) {
    return track == 0xff ? 16u : track;
}
void Collect(lv_obj_t* root, const lv_obj_class_t* type, std::vector<lv_obj_t*>& out) {
    if (lv_obj_check_type(root, type))
        out.push_back(root);
    for (uint32_t i = 0; i < lv_obj_get_child_count(root); ++i)
        Collect(lv_obj_get_child(root, static_cast<int32_t>(i)), type, out);
}
class EmptyPage : public wavex_ui::UIPage {
   public:
    const char* name() const override { return "Test root"; }
    void onEnter(lv_obj_t* parent) override { root_ = lv_obj_create(parent); }
    void onExit() override {
        lv_obj_delete(root_);
        root_ = nullptr;
    }
};
class MixerPageTest : public ::testing::Test {
   protected:
    void SetUp() override {
        static bool initialized = false;
        if (!initialized) {
            lv_init();
            lv_tick_set_cb(Tick);
            auto* display = lv_display_create(1280, 720);
            lv_display_set_buffers(
                display, pixels, nullptr, sizeof(pixels), LV_DISPLAY_RENDER_MODE_DIRECT);
            lv_display_set_flush_cb(display, Flush);
            wavex_ui::UINavigator::instance().push(std::make_shared<EmptyPage>());
            initialized = true;
        }
        alive = respond = true;
        meters_fresh = false;
        subscriptions = unsubscriptions = 0;
        targets = {};
        request = {};
        wavex_ui::mixerSolo.Select(0xff);
        wavex_ui::setCurrentTrack(0);
        page_ = std::make_shared<wavex_ui::UIMixerPage>();
        wavex_ui::UINavigator::instance().push(page_);
    }
    void TearDown() override { wavex_ui::UINavigator::instance().pop(); }
    void Advance(int count = 20) {
        for (int i = 0; i < count; ++i) {
            tick += 50;
            lv_timer_handler();
            lv_refr_now(nullptr);
        }
    }
    bool Command(const char* command) {
        char out[256];
        return page_->consoleCommand(command, out, sizeof(out));
    }
    std::shared_ptr<wavex_ui::UIMixerPage> page_;
};
}  // namespace
bool inter_mcu_backend_link_alive() {
    return alive;
}
esp_err_t inter_mcu_request_mix_state(const WaveX::Protocol::MixStateRequest& r) {
    request = r;
    return 0;
}
bool inter_mcu_get_mix_state(WaveX::Protocol::MixStateMessage* out) {
    if (!respond || !request.request_id)
        return false;
    *out = targets[Index(request.track)];
    out->request_id = request.request_id;
    out->track = request.track;
    out->valid = 1;
    return true;
}
esp_err_t inter_mcu_send_mix_op(uint8_t op, uint8_t track, uint16_t value) {
    using namespace WaveX::Protocol;
    if (op == MIX_OP_SUB_METERS)
        ++subscriptions;
    else if (op == MIX_OP_UNSUB_METERS)
        ++unsubscriptions;
    else if (op == MIX_OP_SET_SOLO_MASK)
        solo_mask = value;
    else if (op == MIX_OP_SET_GAIN || op == MIX_OP_SET_MASTER)
        targets[Index(track)].gain = value;
    else if (op == MIX_OP_SET_PAN)
        targets[Index(track)].pan = value;
    else if (op == MIX_OP_SET_MUTE)
        targets[Index(track)].mute = static_cast<uint8_t>(value);
    return 0;
}
TEST_F(MixerPageTest, LayoutHasNineFadersAndControlsRequireFreshReadback) {
    std::vector<lv_obj_t*> faders;
    Collect(page_->root(), &lv_slider_class, faders);
    ASSERT_EQ(faders.size(), 9u);
    EXPECT_FALSE(Command("LEVEL 5000"));
    Advance();
    if (const char* path = std::getenv("WAVEX_MIXER_PREVIEW")) {
        FILE* image = std::fopen(path, "wb");
        ASSERT_NE(image, nullptr);
        std::fprintf(image, "P6\n1280 720\n255\n");
        for (uint16_t pixel: pixels) {
            const unsigned char rgb[] = {
                static_cast<unsigned char>(((pixel >> 11) & 31) * 255 / 31),
                static_cast<unsigned char>(((pixel >> 5) & 63) * 255 / 63),
                static_cast<unsigned char>((pixel & 31) * 255 / 31)};
            std::fwrite(rgb, 1, sizeof(rgb), image);
        }
        EXPECT_EQ(std::fclose(image), 0);
    }
    lv_obj_update_layout(page_->root());
    lv_area_t bounds;
    lv_obj_get_coords(page_->root(), &bounds);
    for (auto* fader: faders) {
        lv_area_t box;
        lv_obj_get_coords(fader, &box);
        EXPECT_GE(lv_area_get_width(&box), 48);
        EXPECT_GE(box.x1, bounds.x1);
        EXPECT_LE(box.x2, bounds.x2);
        EXPECT_GE(box.y1, bounds.y1);
        EXPECT_LE(box.y2, bounds.y2);
        EXPECT_FALSE(lv_obj_has_state(fader, LV_STATE_DISABLED));
    }
    ASSERT_TRUE(Command("SELECT 0"));  // master
    ASSERT_TRUE(Command("LEVEL 5400"));
    EXPECT_EQ(targets[16].gain, 5400);
    EXPECT_FALSE(Command("LEVEL 5300"));  // wait for this edit's readback
    Advance();
    EXPECT_TRUE(Command("LEVEL 5300"));
    respond = false;
    Advance(50);
    EXPECT_FALSE(Command("LEVEL 5200"));
    alive = false;
    Advance();
    for (auto* fader: faders)
        EXPECT_TRUE(lv_obj_has_state(fader, LV_STATE_DISABLED));
}
TEST_F(MixerPageTest, PagingPreservesTrackIdentityAndSoloDoesNotOverwriteMutes) {
    Advance();
    ASSERT_TRUE(Command("MUTE 1"));
    Advance();
    ASSERT_TRUE(Command("SOLO 1"));
    EXPECT_EQ(solo_mask, 1);
    EXPECT_TRUE(wavex_ui::mixerSolo.Contains(0));
    EXPECT_EQ(targets[0].mute, 1);
    ASSERT_TRUE(Command("SELECT 16"));
    EXPECT_EQ(wavex_ui::getCurrentTrack(), 15);
    EXPECT_FALSE(Command("LEVEL 5400"));
    Advance();
    ASSERT_TRUE(Command("LEVEL 5400"));
    EXPECT_EQ(targets[15].gain, 5400);
    EXPECT_EQ(targets[0].gain, 6000);
    ASSERT_TRUE(Command("SOLO 1"));
    EXPECT_EQ(solo_mask, 0x8000);
    EXPECT_EQ(targets[0].mute, 1);
}
TEST_F(MixerPageTest, UnchangedMixerSnapshotsDoNotRedrawAndExitDeletesTimer) {
    Advance(40);
    flushes = 0;
    Advance(20);
    EXPECT_EQ(flushes, 0u);
    wavex_ui::UINavigator::instance().pop();
    request = {};
    Advance(20);
    EXPECT_EQ(request.request_id, 0u);
}

bool inter_mcu_get_mix_meters(WaveX::Protocol::MixMetersMessage* out) {
    *out = meters;
    return meters_fresh;
}
TEST_F(MixerPageTest, MetersRenewWhileVisibleAndClearStaleLevels) {
    meters_fresh = true;
    meters = {};
    meters.peak[0] = 100;
    Advance(30);
    EXPECT_GE(subscriptions, 2u);
    std::vector<lv_obj_t*> bars;
    Collect(page_->root(), &lv_bar_class, bars);
    ASSERT_EQ(bars.size(), 8u);
    EXPECT_EQ(lv_bar_get_value(bars[0]), 100);
    EXPECT_EQ(lv_bar_get_value(bars[7]), 0);
    meters_fresh = false;
    Advance();
    EXPECT_EQ(lv_bar_get_value(bars[0]), 0);
    wavex_ui::UINavigator::instance().pop();
    EXPECT_EQ(unsubscriptions, 1u);
    const auto previous = subscriptions;
    Advance(30);
    EXPECT_EQ(subscriptions, previous);
}

esp_err_t inter_mcu_send_project_op(const WaveX::Protocol::ProjectOpMessage&) {
    return ESP_OK;
}
