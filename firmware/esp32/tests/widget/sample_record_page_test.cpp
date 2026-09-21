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
