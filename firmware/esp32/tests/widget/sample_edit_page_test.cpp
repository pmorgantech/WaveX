#include <gtest/gtest.h>

#include "envelope_cache.h"
#include "inter_mcu.h"
#include "ui/current_sample.h"
#include "ui/multi_touch_input.h"
#include "ui/ui_navigator.h"
#include "ui/ui_sample_edit_page.h"

#include <cstdlib>
#include <cstring>
#include <vector>
using namespace WaveX::Protocol;
namespace {
uint16_t pixels[1280 * 800];
uint32_t ticks = 0;
SampleMetadata meta;
SampleSeamStatus status;
std::vector<SampleSeamRequest> requests;
unsigned edits = 0;
bool acknowledge = true;
uint32_t Tick() {
    return ticks;
}
void Flush(lv_display_t* display, const lv_area_t*, uint8_t*) {
    lv_display_flush_ready(display);
}
class EmptyPage : public wavex_ui::UIPage {
   public:
    const char* name() const override { return "Root"; }
    void onEnter(lv_obj_t*) override {}
};
class SampleEditPageTest : public ::testing::Test {
   protected:
    std::shared_ptr<wavex_ui::UIPage> page;
    void SetUp() override {
        static bool initialized = false;
        if (!initialized) {
            lv_init();
            lv_tick_set_cb(Tick);
            auto* display = lv_display_create(1280, 800);
            lv_display_set_buffers(
                display, pixels, nullptr, sizeof(pixels), LV_DISPLAY_RENDER_MODE_DIRECT);
            lv_display_set_flush_cb(display, Flush);
            wavex_ui::UINavigator::instance().push(std::make_shared<EmptyPage>());
            initialized = true;
        }
        meta = {};
        meta.sample_id = 1234;
        meta.generation = 3;
        meta.sample_rate = 48000;
        meta.total_frames = meta.end_frame = 48000;
        meta.loop_start = 512;
        meta.loop_end = 4096;
        meta.channels = 2;
        meta.loop_enabled = 1;
        status = {};
        edits = 0;
        requests.clear();
        acknowledge = true;
        wavex_ui::setCurrentSampleId(1234);
        page = wavex_ui::createSampleEditPage();
        wavex_ui::UINavigator::instance().push(page);
        Advance(1);
    }
    void TearDown() override { wavex_ui::UINavigator::instance().pop(); }
    void Advance(unsigned n) {
        while (n--) {
            ticks += 50;
            lv_timer_handler();
        }
    }
    void Focus(unsigned n) {
        char arg[20], reply[100];
        snprintf(arg, sizeof(arg), "FOCUS %u", n);
        ASSERT_TRUE(page->consoleCommand(arg, reply, sizeof(reply)));
    }
    void Adjust(int n) {
        wavex_ui::InputEvent e{};
        e.type = wavex_ui::InputType::EncoderRight;
        e.delta = n;
        page->onInput(e);
        Advance(1);
    }
    std::string State() {
        char out[512]{};
        page->consoleState(out, sizeof(out), 0);
        return out;
    }
};
}  // namespace
namespace wavex_ui {
void statusStripCreate(lv_obj_t*) {}
void EnsureEnvelopeCacheInitialised() {
    GetEnvelopeCache().init(32768, {std::malloc, std::free});
}
EnvelopePanel::Link EspEnvelopeLink() {
    return {};
}
std::shared_ptr<UIPage> createSamplePickerPage() {
    return std::make_shared<EmptyPage>();
}
std::shared_ptr<UIPage> createSampleFilePage(uint16_t, bool) {
    return std::make_shared<EmptyPage>();
}
}  // namespace wavex_ui
bool inter_mcu_get_sample_meta(uint16_t id, SampleMetadata* out) {
    if (id != meta.sample_id)
        return false;
    *out = meta;
    return true;
}
esp_err_t inter_mcu_send_sample_edit(uint16_t,
                                     bool loop,
                                     int16_t gain,
                                     uint32_t start,
                                     uint32_t end,
                                     uint32_t ls,
                                     uint32_t le,
                                     uint16_t fi,
                                     uint16_t fo,
                                     uint8_t xf,
                                     uint8_t mode) {
    ++edits;
    if (acknowledge) {
        meta.loop_enabled = loop;
        meta.gain_db_x10 = gain;
        meta.start_frame = start;
        meta.end_frame = end;
        meta.loop_start = ls;
        meta.loop_end = le;
        meta.fade_in_ms = fi;
        meta.fade_out_ms = fo;
        meta.loop_crossfade_ms = xf;
        if (meta.channel_mode != mode)
            ++meta.generation;
        meta.channel_mode = mode;
    }
    return ESP_OK;
}
esp_err_t inter_mcu_send_sample_audition(uint16_t) {
    return ESP_OK;
}
esp_err_t inter_mcu_send_sample_stop_req() {
    return ESP_OK;
}
esp_err_t inter_mcu_send_sample_seam_request(const SampleSeamRequest& r) {
    requests.push_back(r);
    return ESP_OK;
}
bool inter_mcu_get_sample_seam_status(SampleSeamStatus* out) {
    *out = status;
    return true;
}
TEST_F(SampleEditPageTest, CrossfadeSurvivesDelayedReadbackAndExposesSeamAction) {
    Focus(7);
    acknowledge = false;
    Adjust(20);
    EXPECT_NE(State().find("xf=20"), std::string::npos);
    Advance(4);
    EXPECT_NE(State().find("xf=20"), std::string::npos);
    meta.loop_crossfade_ms = 20;
    acknowledge = true;
    Advance(4);
    EXPECT_NE(State().find("xf=20"), std::string::npos);
    EXPECT_EQ(page->getShiftedSoftkeys()[2].label, "Check Seam");
}
TEST_F(SampleEditPageTest, SnapCorrelatesReplyAndNeverRetriesOnTimeout) {
    Focus(3);
    page->getShiftedSoftkeys()[2].onPress();
    ASSERT_EQ(requests.size(), 1u);
    EXPECT_EQ(requests[0].generation, 3);
    EXPECT_EQ(requests[0].expected.loop_end, 4096u);
    EXPECT_FALSE(page->getShiftedSoftkeys()[2].enabled);
    Adjust(2);
    EXPECT_EQ(edits, 0u);
    status.request_id = requests[0].request_id + 1;
    status.sample_id = 1234;
    Advance(2);
    EXPECT_FALSE(page->getShiftedSoftkeys()[2].enabled);
    Advance(45);
    EXPECT_TRUE(page->getShiftedSoftkeys()[2].enabled);
    EXPECT_EQ(requests.size(), 1u);
    page->getShiftedSoftkeys()[2].onPress();
    ASSERT_EQ(requests.size(), 2u);
    status.request_id = requests.back().request_id;
    status.generation = 3;
    status.error = SAMPLE_SEAM_NO_CROSSING;
    Advance(1);
    EXPECT_TRUE(page->getShiftedSoftkeys()[2].enabled);
    EXPECT_NE(State().find("seamerror=3"), std::string::npos);
}
TEST_F(SampleEditPageTest, ExitDropsPendingIdentityAndTimer) {
    Focus(3);
    page->getShiftedSoftkeys()[2].onPress();
    wavex_ui::UINavigator::instance().pop();
    Advance(50);
    EXPECT_EQ(requests.size(), 1u);
    wavex_ui::UINavigator::instance().push(page);
    Advance(1);
    EXPECT_NE(State().find("seampending=0"), std::string::npos);
    EXPECT_TRUE(page->getShiftedSoftkeys()[2].enabled);
}

esp_err_t inter_mcu_request_sample_meta(uint16_t) {
    return ESP_OK;
}

namespace {
lv_obj_t* FindLabel(lv_obj_t* root, const char* text) {
    if (lv_obj_check_type(root, &lv_label_class) && !std::strcmp(lv_label_get_text(root), text))
        return root;
    for (uint32_t i = 0; i < lv_obj_get_child_count(root); ++i)
        if (auto* found = FindLabel(lv_obj_get_child(root, i), text))
            return found;
    return nullptr;
}
lv_point_t Centre(lv_obj_t* obj) {
    lv_area_t a;
    lv_obj_get_coords(obj, &a);
    return {(a.x1 + a.x2) / 2, (a.y1 + a.y2) / 2};
}
}  // namespace
TEST_F(SampleEditPageTest, PanelEncoderPushSelectsTheNextParameter) {
    wavex_ui::InputEvent e{};
    e.type = wavex_ui::InputType::ButtonPress;
    e.source_id = static_cast<uint8_t>(wavex_ui::PanelKey::NavBPush);
    page->onInput(e);
    EXPECT_NE(State().find("focus=1"), std::string::npos);
    e.type = wavex_ui::InputType::EncoderLeft;
    e.delta = 3;
    page->onInput(e);
    Advance(15);
    EXPECT_LT(meta.end_frame, 48000u);
    EXPECT_EQ(meta.start_frame, 0u);
}
TEST_F(SampleEditPageTest, TouchSelectsMarkerTileForTheEncoderWithoutEditing) {
    auto* label = FindLabel(lv_screen_active(), "END");
    ASSERT_NE(label, nullptr);
    wavex_ui::MultiTouchInput touch;
    ASSERT_TRUE(touch.init(lv_display_get_default()));
    wavex_ui::TouchContact point{1, Centre(label)};
    touch.update(&point, 1);
    touch.update(nullptr, 0);
    touch.deinit();
    EXPECT_NE(State().find("focus=1"), std::string::npos);
    EXPECT_EQ(edits, 0u);
}
TEST_F(SampleEditPageTest, LoopHandleRemainsDraggableUntilRelease) {
    auto* label = FindLabel(lv_screen_active(), "LS");
    ASSERT_NE(label, nullptr);
    auto* handle = lv_obj_get_parent(label);
    wavex_ui::MultiTouchInput touch;
    ASSERT_TRUE(touch.init(lv_display_get_default()));
    wavex_ui::TouchContact point{1, Centre(handle)};
    touch.update(&point, 1);
    point.point.x += 12;
    touch.update(&point, 1);
    Advance(3);
    const uint32_t first = meta.loop_start;
    EXPECT_GT(first, 512u);
    EXPECT_FALSE(lv_obj_has_flag(handle, LV_OBJ_FLAG_HIDDEN));
    point.point.x += 12;
    touch.update(&point, 1);
    Advance(3);
    EXPECT_GT(meta.loop_start, first);
    touch.update(nullptr, 0);
    touch.deinit();
}

TEST_F(SampleEditPageTest, SelectingVisibleTileKeepsItUnderTheFinger) {
    Focus(7);
    Advance(1);
    auto* label = FindLabel(lv_screen_active(), "GAIN");
    ASSERT_NE(label, nullptr);
    const auto before = Centre(label);
    const auto card_before = Centre(lv_obj_get_parent(label));
    wavex_ui::MultiTouchInput touch;
    ASSERT_TRUE(touch.init(lv_display_get_default()));
    wavex_ui::TouchContact point{1, before};
    touch.update(&point, 1);
    Advance(1);
    EXPECT_EQ(Centre(lv_obj_get_parent(label)).x, card_before.x);
    touch.update(nullptr, 0);
    touch.deinit();
    EXPECT_NE(State().find("focus=4"), std::string::npos);
}

TEST_F(SampleEditPageTest, ChannelControlPublishesCompleteEditAndSeamUsesConfirmedRevision) {
    Focus(8);
    Adjust(2);
    EXPECT_EQ(meta.channel_mode, SAMPLE_CH_RIGHT);
    EXPECT_EQ(meta.generation, 4);
    EXPECT_NE(State().find("channelmode=2"), std::string::npos);
    EXPECT_NE(State().find("wavegen=4"), std::string::npos);
    Focus(0);
    page->getShiftedSoftkeys()[2].onPress();
    ASSERT_FALSE(requests.empty());
    EXPECT_EQ(requests.back().expected.channel_mode, SAMPLE_CH_RIGHT);
    EXPECT_EQ(requests.back().generation, 4);
}

TEST_F(SampleEditPageTest, DeferredParameterCardsInitializeFromLiveStateAndSurvivePaging) {
    EXPECT_EQ(FindLabel(lv_screen_active(), "GAIN"), nullptr);
    EXPECT_EQ(FindLabel(lv_screen_active(), "CHANNEL"), nullptr);
    meta.gain_db_x10 = -120;
    ++meta.generation;
    Advance(5);
    Focus(4);
    Advance(1);
    auto* gain = FindLabel(lv_screen_active(), "GAIN");
    ASSERT_NE(gain, nullptr);
    EXPECT_NE(FindLabel(lv_obj_get_parent(gain), "-12.0 dB"), nullptr);
    Focus(8);
    Adjust(2);
    EXPECT_EQ(meta.channel_mode, SAMPLE_CH_RIGHT);
    Focus(0);
    Focus(4);
    EXPECT_EQ(FindLabel(lv_screen_active(), "GAIN"), gain);
    EXPECT_FALSE(lv_obj_has_flag(lv_obj_get_parent(gain), LV_OBJ_FLAG_HIDDEN));
}
