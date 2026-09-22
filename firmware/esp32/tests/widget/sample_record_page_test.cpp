#include <gtest/gtest.h>

#include "inter_mcu.h"
#include "ui/ui_navigator.h"
#include "ui/ui_sample_record_page.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
using namespace WaveX::Protocol;
namespace wavex_ui {
void statusStripCreate(lv_obj_t*) {}
}  // namespace wavex_ui
namespace {
uint16_t pixels[1280 * 800];
uint32_t ticks = 0, read_id = 0;
bool alive = true, respond = true;
uint16_t assigned_sample = 0;
bool assigned_keyboard = false;
RecordStatusMessage status;
std::vector<RecordOpMessage> mutations;
uint32_t Tick() {
    return ticks;
}
void Flush(lv_display_t* display, const lv_area_t*, uint8_t*) {
    lv_display_flush_ready(display);
}
class EmptyPage : public wavex_ui::UIPage {
   public:
    const char* name() const override { return "Root"; }
    void onEnter(lv_obj_t* parent) override { root_ = lv_obj_create(parent); }
    void onExit() override {
        lv_obj_delete(root_);
        root_ = nullptr;
    }
};
class SampleRecordPageTest : public ::testing::Test {
   protected:
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
        alive = respond = true;
        status = {};
        mutations.clear();
        assigned_sample = 0;
        page = wavex_ui::createSampleRecordPage();
        wavex_ui::UINavigator::instance().push(page);
        Advance(5);
    }
    void TearDown() override { wavex_ui::UINavigator::instance().pop(); }
    void Advance(unsigned count) {
        for (unsigned i = 0; i < count; ++i) {
            ticks += 100;
            lv_timer_handler();
        }
    }
    void Name() {
        char reply[64];
        ASSERT_TRUE(page->consoleCommand("NAME Night", reply, sizeof(reply)));
    }
    void Press(unsigned key) {
        const auto keys = page->getSoftkeys();
        ASSERT_TRUE(keys[key].enabled);
        ASSERT_TRUE(keys[key].onPress);
        keys[key].onPress();
        Advance(1);
    }
    std::shared_ptr<wavex_ui::UIPage> page;
};
}  // namespace
namespace wavex_ui {
std::shared_ptr<UIPage> createRecordedSampleMap(bool keyboard, uint16_t sample) {
    assigned_sample = sample;
    assigned_keyboard = keyboard;
    return std::make_shared<EmptyPage>();
}
}  // namespace wavex_ui
bool inter_mcu_backend_link_alive() {
    return alive;
}
esp_err_t inter_mcu_send_record_op(const RecordOpMessage& request) {
    read_id = request.request_id;
    if (request.op != REC_GET) {
        mutations.push_back(request);
        status.active_request_id = request.request_id;
    }
    return ESP_OK;
}
bool inter_mcu_get_record_status(RecordStatusMessage* out) {
    if (!respond)
        return false;
    *out = status;
    out->request_id = read_id;
    return true;
}

TEST_F(SampleRecordPageTest, BothSourcesUseTheSameArmContractAndWaitForAcknowledgement) {
    for (unsigned source = 0; source < 4; ++source) {
        char command[24], reply[32];
        std::snprintf(command, sizeof(command), "SOURCE %u", source);
        ASSERT_TRUE(page->consoleCommand(command, reply, sizeof(reply)));
        Press(1);
        ASSERT_EQ(mutations.size(), source + 1);
        EXPECT_EQ(mutations.back().source, source);
        EXPECT_EQ(mutations.back().op, REC_ARM);
        EXPECT_FALSE(page->getSoftkeys()[1].enabled);
        Advance(20);
        EXPECT_EQ(mutations.size(), source + 1);  // Polling must not re-arm.
        status.completed_request_id = mutations.back().request_id;
        status.active_request_id = 0;
        status.error = REC_NO_MEMORY;
        Advance(4);
    }
}
TEST_F(SampleRecordPageTest, SavedTakeUsesDoneAndCarriesBackendIdentity) {
    status.state = REC_READY;
    status.take_id = 42;
    status.sample_id = 1234;
    status.frames = 48000;
    status.max_frames = 48000;
    std::strcpy(status.path, "/wavex/recordings/Take.wav");
    Advance(5);
    EXPECT_EQ(page->getSoftkeys()[5].label, "Done");
    EXPECT_FALSE(page->getSoftkeys()[4].enabled);
    Press(5);
    ASSERT_EQ(mutations.size(), 1u);
    EXPECT_EQ(mutations[0].take_id, 42u);
    EXPECT_EQ(mutations[0].op, REC_DISCARD);
}
TEST_F(SampleRecordPageTest, DisconnectAndLeavingPageNeverReplayOrDiscardTake) {
    Press(1);
    alive = false;
    Advance(3);
    EXPECT_FALSE(page->getSoftkeys()[1].enabled);
    alive = true;
    Advance(10);
    EXPECT_EQ(mutations.size(), 1u);
    respond = false;
    Advance(20);
    EXPECT_FALSE(page->getSoftkeys()[1].enabled);
}

TEST_F(SampleRecordPageTest, AssignmentNavigatesOnlyAfterSuccessfulDoneAcknowledgement) {
    status.state = REC_READY;
    status.take_id = 42;
    status.sample_id = 1234;
    status.frames = status.max_frames = 48000;
    Advance(5);
    EXPECT_FALSE(page->getShiftedSoftkeys()[2].enabled);
    std::strcpy(status.path, "/wavex/recordings/Take.wav");
    Advance(5);
    auto key = page->getShiftedSoftkeys()[2];
    ASSERT_TRUE(key.enabled);
    key.onPress();
    ASSERT_EQ(mutations.size(), 1u);
    Advance(5);
    EXPECT_EQ(assigned_sample, 0);
    status = {};
    status.completed_request_id = mutations.back().request_id;
    status.completed_op = REC_DISCARD;
    Advance(5);
    EXPECT_EQ(assigned_sample, 1234);
    EXPECT_TRUE(assigned_keyboard);
    wavex_ui::UINavigator::instance().pop();  // Return to Record for fixture teardown.
}

namespace {
lv_obj_t* FindType(lv_obj_t* obj, const lv_obj_class_t* type) {
    if (lv_obj_check_type(obj, type))
        return obj;
    for (uint32_t i = 0; i < lv_obj_get_child_count(obj); ++i)
        if (auto* found = FindType(lv_obj_get_child(obj, i), type))
            return found;
    return nullptr;
}
}  // namespace
TEST_F(SampleRecordPageTest, DeferredKeyboardReopensAndRetainsTheTakeName) {
    EXPECT_EQ(FindType(lv_screen_active(), &lv_keyboard_class), nullptr);
    auto* input = FindType(lv_screen_active(), &lv_textarea_class);
    ASSERT_NE(input, nullptr);
    lv_obj_send_event(input, LV_EVENT_CLICKED, nullptr);
    Advance(1);
    auto* keyboard = FindType(lv_screen_active(), &lv_keyboard_class);
    ASSERT_NE(keyboard, nullptr);
    EXPECT_FALSE(lv_obj_has_flag(keyboard, LV_OBJ_FLAG_HIDDEN));
    lv_textarea_set_text(input, "Retained take");
    lv_obj_send_event(keyboard, LV_EVENT_READY, nullptr);
    EXPECT_TRUE(lv_obj_has_flag(keyboard, LV_OBJ_FLAG_HIDDEN));
    lv_obj_send_event(input, LV_EVENT_CLICKED, nullptr);
    EXPECT_EQ(FindType(lv_screen_active(), &lv_keyboard_class), keyboard);
    EXPECT_FALSE(lv_obj_has_flag(keyboard, LV_OBJ_FLAG_HIDDEN));
    EXPECT_STREQ(lv_textarea_get_text(input), "Retained take");
    lv_obj_send_event(keyboard, LV_EVENT_CANCEL, nullptr);
    EXPECT_TRUE(lv_obj_has_flag(keyboard, LV_OBJ_FLAG_HIDDEN));
    EXPECT_TRUE(mutations.empty());
}
