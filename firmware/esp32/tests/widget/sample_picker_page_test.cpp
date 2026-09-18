#include <gtest/gtest.h>

#include "inter_mcu.h"
#include "ui/current_sample.h"
#include "ui/current_track.h"
#include "ui/ui_navigator.h"
#include "ui/ui_sample_manager_page.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>

using namespace WaveX::Protocol;
namespace wavex_ui {
void statusStripCreate(lv_obj_t*) {}
}  // namespace wavex_ui
namespace {
uint16_t pixels[1280 * 720];
std::array<SampleMetadata, 10> samples;
uint16_t first = 0, requested = 0;
unsigned mutations = 0;
bool alive = true, removed = false, deliver = true;
uint32_t ticks = 0, revision = 1;
uint32_t Tick() {
    return ticks;
}
void Flush(lv_display_t* display, const lv_area_t*, uint8_t*) {
    lv_display_flush_ready(display);
}
class Root : public wavex_ui::UIPage {
   public:
    const char* name() const override { return "Root"; }
    void onEnter(lv_obj_t* parent) override { root_ = lv_obj_create(parent); }
    void onExit() override {
        lv_obj_delete(root_);
        root_ = nullptr;
    }
};
class SamplePickerTest : public ::testing::Test {
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
            wavex_ui::UINavigator::instance().push(std::make_shared<Root>());
            initialized = true;
        }
        for (unsigned i = 0; i < samples.size(); ++i) {
            samples[i] = {};
            samples[i].sample_id = static_cast<uint16_t>(i + 1);
            samples[i].flags = SAMPLE_META_RESIDENT;
            samples[i].sample_rate = 48000;
            samples[i].total_frames = 48000;
            samples[i].end_frame = 48000;
            samples[i].channels = 1;
            samples[i].bits_per_sample = 16;
            std::snprintf(samples[i].name, sizeof(samples[i].name), "sample-%u.wav", i + 1);
        }
        first = requested = 0;
        mutations = 0;
        removed = false;
        alive = deliver = true;
        wavex_ui::setCurrentSampleId(9);
        wavex_ui::setCurrentTrack(4);
        page = std::make_shared<wavex_ui::UISampleManagerPage>(true);
        wavex_ui::UINavigator::instance().push(page);
    }
    void TearDown() override {
        if (wavex_ui::UINavigator::instance().active() == page)
            wavex_ui::UINavigator::instance().pop();
        EXPECT_EQ(mutations, 0u);
        EXPECT_EQ(wavex_ui::getCurrentTrack(), 4u);
    }
    void Press(unsigned i) {
        auto keys = page->getSoftkeys();
        ASSERT_TRUE(keys[i].onPress);
        keys[i].onPress();
    }
    std::shared_ptr<wavex_ui::UISampleManagerPage> page;
};
}  // namespace
bool inter_mcu_backend_link_alive() {
    return alive;
}
bool inter_mcu_get_sample_meta(uint16_t id, SampleMetadata* out) {
    if (removed || id == 0 || id > samples.size())
        return false;
    *out = samples[id - 1];
    return true;
}
size_t inter_mcu_get_sample_meta_page(SampleMetadata* out,
                                      size_t cap,
                                      uint16_t* total,
                                      uint16_t* start) {
    *total = static_cast<uint16_t>(samples.size());
    *start = first;
    const auto count = std::min(cap, samples.size() - first);
    std::copy_n(samples.begin() + first, count, out);
    return count;
}
bool inter_mcu_get_track_binding(uint8_t, TrackBindingMessage*) {
    return false;
}
esp_err_t inter_mcu_request_track_binding(uint8_t) {
    return ESP_OK;
}
esp_err_t inter_mcu_request_sample_meta_page(uint16_t start, uint8_t) {
    requested = start;
    if (deliver)
        first = start;
    return ESP_OK;
}
esp_err_t inter_mcu_request_sample_mem_status() {
    return ESP_OK;
}
uint32_t inter_mcu_sample_pool_revision() {
    return 1;
}
uint32_t inter_mcu_sample_cache_revision() {
    return revision;
}
esp_err_t inter_mcu_send_sample_select(uint16_t, uint8_t) {
    ++mutations;
    return ESP_OK;
}
esp_err_t inter_mcu_send_sample_unload(uint16_t) {
    ++mutations;
    return ESP_OK;
}
TEST_F(SamplePickerTest, CancelPreservesSelectionAndHasNoDestructiveActions) {
    auto keys = page->getSoftkeys();
    EXPECT_EQ(keys[0].label, "Cancel");
    EXPECT_EQ(keys[1].label, "Select");
    EXPECT_FALSE(keys[2].onPress);
    Press(4);
    Press(0);
    EXPECT_EQ(wavex_ui::getCurrentSampleId(), 9u);
}
TEST_F(SamplePickerTest, SelectReturnsToEditorWithoutAssigningATrack) {
    Press(4);
    Press(1);
    EXPECT_EQ(wavex_ui::getCurrentSampleId(), 2u);
    EXPECT_NE(wavex_ui::UINavigator::instance().active(), page);
}
TEST_F(SamplePickerTest, RemovedAndDisconnectedSamplesCannotBeSelected) {
    removed = true;
    Press(1);
    EXPECT_EQ(wavex_ui::getCurrentSampleId(), 9u);
    removed = false;
    alive = false;
    Press(1);
    EXPECT_EQ(wavex_ui::getCurrentSampleId(), 9u);
}
TEST_F(SamplePickerTest, InFlightPageCannotSelectAnOldRow) {
    for (unsigned i = 0; i < 7; ++i)
        Press(4);
    deliver = false;
    Press(4);
    ASSERT_EQ(requested, 8u);
    Press(1);
    EXPECT_EQ(wavex_ui::getCurrentSampleId(), 9u);
    first = requested;
    ++revision;
    ticks += 300;
    lv_timer_handler();
    Press(4);
    Press(1);
    EXPECT_EQ(wavex_ui::getCurrentSampleId(), 10u);
}
