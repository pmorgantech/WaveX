#include <gtest/gtest.h>

#include "inter_mcu.h"
#include "ui/mixer_solo.h"
#include "ui/ui_navigator.h"
#include "ui/ui_pattern_slots_page.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
using namespace WaveX::Protocol;
namespace wavex_ui {
void statusStripCreate(lv_obj_t*) {}
std::shared_ptr<UIPage> createPatternFilesPage() {
    return {};
}
}  // namespace wavex_ui
namespace {
uint16_t pixels[1280 * 720];
uint32_t ticks = 0, read_id = 0;
bool alive = true, respond = true;
SeqSlotStatusMessage status;
std::vector<SeqSlotOpMessage> mutations;
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
class PatternSlotsPageTest : public ::testing::Test {
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
        status = {};
        mutations.clear();
        page = std::make_shared<wavex_ui::UIPatternSlotsPage>();
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
    std::shared_ptr<wavex_ui::UIPatternSlotsPage> page;
};
}  // namespace
bool inter_mcu_backend_link_alive() {
    return alive;
}
esp_err_t inter_mcu_send_seq_slot_op(const SeqSlotOpMessage& request) {
    read_id = request.request_id;
    status.slot = request.slot;
    if (request.op != SEQ_SLOT_GET) {
        mutations.push_back(request);
        status.busy = 1;
        status.active_request_id = request.request_id;
    }
    return ESP_OK;
}
bool inter_mcu_get_seq_slot_status(SeqSlotStatusMessage* out) {
    if (!respond)
        return false;
    *out = status;
    out->request_id = read_id;
    return true;
}

TEST_F(PatternSlotsPageTest, CopyNamesTargetAndWaitsForRetainedCompletion) {
    Name();
    char reply[64];
    ASSERT_TRUE(page->consoleCommand("SLOT 128", reply, sizeof(reply)));
    Advance(5);
    Press(2);
    ASSERT_EQ(mutations.size(), 1u);
    EXPECT_EQ(mutations[0].op, SEQ_SLOT_COPY);
    EXPECT_EQ(mutations[0].slot, 127);
    EXPECT_STREQ(mutations[0].name, "Night");
    EXPECT_FALSE(page->getSoftkeys()[2].enabled);
    Advance(300);
    EXPECT_FALSE(page->getSoftkeys()[2].enabled);
    alive = false;
    Advance(3);
    alive = true;
    Advance(5);
    EXPECT_EQ(mutations.size(), 1u);
    status.busy = 0;
    status.active_request_id = 0;
    status.completed_request_id = mutations[0].request_id;
    status.completed_op = SEQ_SLOT_COPY;
    status.used = 1;
    std::strcpy(status.name, "Night");
    Advance(5);
    EXPECT_FALSE(page->getSoftkeys()[2].enabled);
    EXPECT_TRUE(page->getSoftkeys()[4].enabled);
}
TEST_F(PatternSlotsPageTest, OccupiedEmptyStaleAndReentryControlReadiness) {
    EXPECT_TRUE(page->getSoftkeys()[1].enabled);
    EXPECT_FALSE(page->getSoftkeys()[4].enabled);
    status.used = 1;
    std::strcpy(status.name, "Verse");
    Advance(5);
    EXPECT_FALSE(page->getSoftkeys()[1].enabled);
    EXPECT_TRUE(page->getSoftkeys()[4].enabled);
    respond = false;
    Advance(20);
    EXPECT_FALSE(page->getSoftkeys()[4].enabled);
    const auto before = read_id;
    wavex_ui::UINavigator::instance().pop();
    Advance(20);
    EXPECT_EQ(read_id, before);
    wavex_ui::UINavigator::instance().push(page);
    EXPECT_FALSE(page->getSoftkeys()[4].enabled);
}
TEST_F(PatternSlotsPageTest, Preview) {
    status.used = 1;
    std::strcpy(status.name, "Verse A");
    Advance(5);
    lv_refr_now(nullptr);
    if (const char* path = std::getenv("WAVEX_SLOT_PREVIEW")) {
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
}

TEST_F(PatternSlotsPageTest, LaunchIsSingleMutationAndSurvivesQueuedNavigation) {
    status.used = 1;
    Advance(5);
    Press(4);
    ASSERT_EQ(mutations.size(), 1u);
    EXPECT_EQ(mutations[0].op, SEQ_SLOT_LAUNCH);
    status.queued_pattern = 1;
    Advance(200);
    EXPECT_EQ(mutations.size(), 1u);
    EXPECT_FALSE(page->getSoftkeys()[4].enabled);
    EXPECT_TRUE(page->getSoftkeys()[0].enabled);
    status.busy = 0;
    status.active_request_id = 0;
    status.queued_pattern = 0xff;
    status.completed_request_id = mutations[0].request_id;
    status.completed_op = SEQ_SLOT_LAUNCH;
    status.error = SEQ_SLOT_CANCELLED;
    Advance(5);
    EXPECT_TRUE(page->getSoftkeys()[4].enabled);
}
