#include <gtest/gtest.h>

#include "inter_mcu.h"
#include "ui/current_track.h"
#include "ui/ui_navigator.h"
#include "ui/ui_play_page.h"

#include <vector>
namespace {
uint16_t pixels[1280 * 800];
uint32_t ticks = 0;
std::vector<uint8_t> on_notes, off_notes;
std::vector<uint8_t> on_tracks, off_tracks;
esp_err_t on_result = ESP_OK, off_result = ESP_OK;
bool link_alive = true, respond_controls = true;
std::vector<WaveX::Protocol::ControlChangeMessage> controls;
WaveX::Protocol::InstEditSyncMessage filter_reply;
WaveX::Protocol::InstModSyncMessage envelope_reply;
uint32_t Tick() {
    return ticks;
}
void Flush(lv_display_t* display, const lv_area_t*, uint8_t*) {
    lv_display_flush_ready(display);
}
lv_obj_t* FindType(lv_obj_t* obj, const lv_obj_class_t* type) {
    if (lv_obj_check_type(obj, type))
        return obj;
    for (uint32_t i = 0; i < lv_obj_get_child_count(obj); ++i)
        if (auto* found = FindType(lv_obj_get_child(obj, i), type))
            return found;
    return nullptr;
}
class EmptyPage : public wavex_ui::UIPage {
   public:
    const char* name() const override { return "Root"; }
    void onEnter(lv_obj_t*) override {}
};
class PlayPageTest : public ::testing::Test {
   protected:
    std::shared_ptr<wavex_ui::UIPage> page;
    lv_obj_t* tabs = nullptr;
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
        on_result = off_result = ESP_OK;
        link_alive = respond_controls = true;
        controls.clear();
        filter_reply = {};
        envelope_reply = {};
        on_notes.clear();
        off_notes.clear();
        on_tracks.clear();
        off_tracks.clear();
        wavex_ui::setCurrentTrack(0);
        page = wavex_ui::createPlayPage();
        wavex_ui::UINavigator::instance().push(page);
        Draw();
        tabs = FindType(lv_screen_active(), &lv_tabview_class);
    }
    void TearDown() override { wavex_ui::UINavigator::instance().pop(); }
    void Draw() {
        ticks += 100;
        lv_timer_handler();
    }
    void Select(unsigned tab) {
        lv_tabview_set_active(tabs, tab, LV_ANIM_OFF);
        lv_obj_send_event(tabs, LV_EVENT_VALUE_CHANGED, nullptr);
        Draw();
    }
};
}  // namespace
namespace wavex_ui {
void statusStripCreate(lv_obj_t*) {}
}  // namespace wavex_ui
bool inter_mcu_backend_link_alive() {
    return link_alive;
}
bool inter_mcu_get_track_binding(uint8_t, WaveX::Protocol::TrackBindingMessage*) {
    return false;
}
esp_err_t inter_mcu_request_track_binding(uint8_t) {
    return ESP_OK;
}
bool inter_mcu_get_sample_meta(uint16_t, WaveX::Protocol::SampleMetadata*) {
    return false;
}
esp_err_t inter_mcu_send_control_change(uint8_t parameter, uint8_t track, uint16_t value) {
    controls.emplace_back(parameter, track, value);
    return ESP_OK;
}
esp_err_t inter_mcu_send_note_on_track(uint8_t note, uint8_t, uint8_t track) {
    on_notes.push_back(note);
    on_tracks.push_back(track);
    return on_result;
}
esp_err_t inter_mcu_send_note_off_track(uint8_t note, uint8_t track) {
    off_notes.push_back(note);
    off_tracks.push_back(track);
    return off_result;
}
TEST_F(PlayPageTest, DeferredKeysBuildOnceAndTabSwitchReleasesHeldNotes) {
    ASSERT_NE(tabs, nullptr);
    auto* keys = lv_obj_get_child(lv_tabview_get_content(tabs), 1);
    EXPECT_EQ(lv_obj_get_child_count(keys), 0u);
    page->getShiftedSoftkeys()[2].onPress();  // Transpose before Keys exists.
    Select(1);
    const auto count = lv_obj_get_child_count(keys);
    EXPECT_GE(count, 25u);
    auto* key = FindType(keys, &lv_button_class);
    ASSERT_NE(key, nullptr);
    lv_obj_send_event(key, LV_EVENT_PRESSED, nullptr);
    ASSERT_EQ(on_notes.size(), 1u);
    EXPECT_EQ(on_notes.front(), 72);
    Select(0);
    EXPECT_EQ(off_notes, on_notes);
    Select(1);
    EXPECT_EQ(lv_obj_get_child_count(keys), count);
    EXPECT_TRUE(lv_obj_is_valid(key));
    lv_obj_send_event(key, LV_EVENT_PRESSED, nullptr);
    lv_obj_send_event(key, LV_EVENT_RELEASED, nullptr);
    EXPECT_EQ(on_notes.size(), 2u);
    EXPECT_EQ(off_notes, on_notes);
}

TEST_F(PlayPageTest, ReleaseRetainsTrackAcrossSelectionChange) {
    Select(1);
    auto* key = FindType(lv_obj_get_child(lv_tabview_get_content(tabs), 1), &lv_button_class);
    ASSERT_NE(key, nullptr);
    lv_obj_send_event(key, LV_EVENT_PRESSED, nullptr);
    wavex_ui::setCurrentTrack(1);
    page->onTrackChanged();
    lv_obj_send_event(key, LV_EVENT_RELEASED, nullptr);
    EXPECT_EQ(on_tracks, std::vector<uint8_t>({0}));
    EXPECT_EQ(off_tracks, on_tracks);
    EXPECT_EQ(off_notes, on_notes);
}

TEST_F(PlayPageTest, AllOffRetriesRejectedRelease) {
    Select(1);
    auto* key = FindType(lv_obj_get_child(lv_tabview_get_content(tabs), 1), &lv_button_class);
    lv_obj_send_event(key, LV_EVENT_PRESSED, nullptr);
    off_result = -1;
    lv_obj_send_event(key, LV_EVENT_RELEASED, nullptr);
    off_result = ESP_OK;
    page->getShiftedSoftkeys()[5].onPress();
    EXPECT_EQ(off_notes.size(), 2u);
}
TEST_F(PlayPageTest, RejectedPressDoesNotBecomeHeld) {
    Select(1);
    auto* key = FindType(lv_obj_get_child(lv_tabview_get_content(tabs), 1), &lv_button_class);
    on_result = -1;
    lv_obj_send_event(key, LV_EVENT_PRESSED, nullptr);
    lv_obj_send_event(key, LV_EVENT_RELEASED, nullptr);
    EXPECT_TRUE(off_notes.empty());
    on_result = ESP_OK;
    lv_obj_send_event(key, LV_EVENT_PRESSED, nullptr);
    lv_obj_send_event(key, LV_EVENT_RELEASED, nullptr);
    EXPECT_EQ(off_notes.size(), 1u);
}

esp_err_t inter_mcu_send_instrument_edit(const WaveX::Protocol::InstEditOpMessage& request) {
    filter_reply = {};
    filter_reply.request_id = request.request_id;
    filter_reply.track = request.track;
    filter_reply.valid = 1;
    filter_reply.sound.cutoff_hz = request.track ? 2000.f : 200.f;
    return ESP_OK;
}
esp_err_t inter_mcu_send_modulator(const WaveX::Protocol::InstModOpMessage& request) {
    envelope_reply = {};
    envelope_reply.request_id = request.request_id;
    envelope_reply.track = request.track;
    envelope_reply.valid = 1;
    return ESP_OK;
}
bool inter_mcu_get_instrument_edit(WaveX::Protocol::InstEditSyncMessage* out) {
    if (!respond_controls)
        return false;
    *out = filter_reply;
    return true;
}
bool inter_mcu_get_modulator(WaveX::Protocol::InstModSyncMessage* out) {
    if (!respond_controls)
        return false;
    *out = envelope_reply;
    return true;
}
TEST_F(PlayPageTest, FirstControlStepUsesBackendValueAndTrackChangeWaitsForReadback) {
    page->getSoftkeys()[4].onPress();
    ASSERT_EQ(controls.size(), 1u);
    EXPECT_NEAR(controls.back().value, 21845 + 1023, 1);
    wavex_ui::setCurrentTrack(1);
    page->onTrackChanged();
    page->getSoftkeys()[4].onPress();
    EXPECT_EQ(controls.size(), 1u);
    Draw();
    page->getSoftkeys()[4].onPress();
    ASSERT_EQ(controls.size(), 2u);
    EXPECT_EQ(controls.back().channel, 1);
    EXPECT_NEAR(controls.back().value, 43690 + 1023, 1);
}
TEST_F(PlayPageTest, MissingAndDisconnectedReadbackCannotAuthorizeEdits) {
    respond_controls = false;
    wavex_ui::setCurrentTrack(1);
    page->onTrackChanged();
    page->getSoftkeys()[4].onPress();
    EXPECT_TRUE(controls.empty());
    link_alive = false;
    Draw();
    page->getSoftkeys()[4].onPress();
    EXPECT_TRUE(controls.empty());
}
