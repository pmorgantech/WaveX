#include <gtest/gtest.h>

#include "inter_mcu.h"
#include "ui/mixer_solo.h"
#include "ui/ui_navigator.h"
#include "ui/ui_project_files_page.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
using namespace WaveX::Protocol;
namespace wavex_ui {
void statusStripCreate(lv_obj_t*) {}
}  // namespace wavex_ui
namespace {
uint16_t pixels[1280 * 720];
uint32_t ticks = 0, read_id = 0;
bool alive = true, respond = true;
ProjectStatusMessage status;
std::vector<ProjectOpMessage> mutations;
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
class ProjectFilesPageTest : public ::testing::Test {
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
        page = std::make_shared<wavex_ui::UIProjectFilesPage>();
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
    std::shared_ptr<wavex_ui::UIProjectFilesPage> page;
};
}  // namespace
bool inter_mcu_backend_link_alive() {
    return alive;
}
esp_err_t inter_mcu_send_project_op(const ProjectOpMessage& request) {
    read_id = request.request_id;
    if (request.op != PROJECT_GET) {
        mutations.push_back(request);
        status.busy = 1;
        status.active_request_id = request.request_id;
        status.active_op = request.op;
    }
    return ESP_OK;
}
bool inter_mcu_get_project_status(ProjectStatusMessage* out) {
    if (!respond)
        return false;
    *out = status;
    out->request_id = read_id;
    return true;
}
TEST_F(ProjectFilesPageTest, LoadAndNewRequireConfirmationAndCancelSendsNothing) {
    lv_refr_now(nullptr);
    if (const char* path = std::getenv("WAVEX_PROJECT_PREVIEW")) {
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
    Name();
    Press(2);
    EXPECT_TRUE(mutations.empty());
    EXPECT_EQ(page->getSoftkeys()[0].label, "Cancel");
    Press(0);
    EXPECT_TRUE(mutations.empty());
    Press(3);
    EXPECT_TRUE(mutations.empty());
    Press(1);
    ASSERT_EQ(mutations.size(), 1u);
    EXPECT_EQ(mutations[0].op, PROJECT_NEW);
}
TEST_F(ProjectFilesPageTest, LongJobAndReconnectPollWithoutReplayingMutation) {
    Name();
    Press(1);
    ASSERT_EQ(mutations.size(), 1u);
    EXPECT_EQ(mutations[0].op, PROJECT_SAVE_COPY);
    const auto id = mutations[0].request_id;
    Advance(300);
    EXPECT_EQ(mutations.size(), 1u);
    EXPECT_FALSE(page->getSoftkeys()[1].enabled);
    alive = false;
    Advance(3);
    alive = true;
    Advance(5);
    EXPECT_FALSE(page->getSoftkeys()[1].enabled);
    EXPECT_EQ(mutations.size(), 1u);
    status.busy = 0;
    status.active_request_id = 0;
    status.active_op = PROJECT_GET;
    status.completed_request_id = id;
    status.completed_op = PROJECT_SAVE_COPY;
    status.progress = 100;
    std::strcpy(status.name, "Night");
    Advance(5);
    EXPECT_TRUE(page->getSoftkeys()[1].enabled);
    EXPECT_EQ(mutations.size(), 1u);
}
TEST_F(ProjectFilesPageTest, StaleStatusDisablesActionsAndExitDeletesItsTimer) {
    EXPECT_TRUE(page->getSoftkeys()[1].enabled);
    respond = false;
    Advance(20);
    EXPECT_FALSE(page->getSoftkeys()[1].enabled);
    const auto before = read_id;
    wavex_ui::UINavigator::instance().pop();
    Advance(20);
    EXPECT_EQ(read_id, before);
    // Restore a page so fixture teardown balances the navigator stack.
    wavex_ui::UINavigator::instance().push(page);
}
TEST(MixerSoloReset, CompletionIntentIsConsumedOnUiAccessBeforeAnotherSelection) {
    wavex_ui::MixerSolo model;
    model.Select(15);
    model.RequestReset();
    EXPECT_EQ(model.Mask(), 0);
    EXPECT_FALSE(model.Active());
    model.Select(3);
    EXPECT_EQ(model.Mask(), 8);
    model.RequestReset();
    model.Select(5);
    EXPECT_EQ(model.Mask(), 32);
}
