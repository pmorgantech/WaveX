#include <gtest/gtest.h>

#include "inter_mcu.h"
#include "ui/ui_navigator.h"
#include "ui/ui_sample_file_page.h"

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
SampleFileStatusMessage status;
std::vector<SampleFileOpMessage> mutations;
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
class SampleFilePageTest : public ::testing::Test {
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
        page = wavex_ui::createSampleFilePage(1234, true);
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
esp_err_t inter_mcu_send_sample_file_op(const SampleFileOpMessage& request) {
    read_id = request.request_id;
    if (request.op != SAMPLE_FILE_GET) {
        mutations.push_back(request);
        status.busy = 1;
        status.active_request_id = request.request_id;
    }
    return ESP_OK;
}
bool inter_mcu_get_sample_file_status(SampleFileStatusMessage* out) {
    if (!respond)
        return false;
    *out = status;
    out->request_id = read_id;
    return true;
}
TEST_F(SampleFilePageTest, LongJobAndReconnectPollWithoutReplayingMutation) {
    Name();
    if (const char* path = std::getenv("WAVEX_SAMPLE_FILE_PREVIEW")) {
        lv_refr_now(nullptr);
        FILE* image = std::fopen(path, "wb");
        ASSERT_NE(image, nullptr);
        std::fprintf(image, "P6\n1280 800\n255\n");
        for (uint16_t pixel: pixels) {
            const unsigned char rgb[] = {
                static_cast<unsigned char>(((pixel >> 11) & 31) * 255 / 31),
                static_cast<unsigned char>(((pixel >> 5) & 63) * 255 / 63),
                static_cast<unsigned char>((pixel & 31) * 255 / 31)};
            std::fwrite(rgb, 1, sizeof(rgb), image);
        }
        EXPECT_EQ(std::fclose(image), 0);
    }
    Press(1);
    ASSERT_EQ(mutations.size(), 1u);
    EXPECT_EQ(mutations[0].op, SAMPLE_FILE_COPY);
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
    status.completed_request_id = id;
    status.completed_op = SAMPLE_FILE_COPY;
    status.progress = 100;
    std::strcpy(status.path, "/Night.wav");
    Advance(5);
    EXPECT_TRUE(page->getSoftkeys()[1].enabled);
    EXPECT_EQ(mutations.size(), 1u);
}
TEST_F(SampleFilePageTest, StaleStatusDisablesActionsAndExitDeletesItsTimer) {
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
TEST_F(SampleFilePageTest, RequestCarriesSampleIdentityAndFailureAllowsExplicitRetry) {
    Name();
    Press(1);
    ASSERT_EQ(mutations.size(), 1u);
    EXPECT_EQ(mutations[0].sample_id, 1234);
    EXPECT_STREQ(mutations[0].name, "Night");
    status.busy = 0;
    status.active_request_id = 0;
    status.completed_request_id = mutations[0].request_id;
    status.completed_op = SAMPLE_FILE_COPY;
    status.error = SAMPLE_FILE_EXISTS;
    Advance(5);
    EXPECT_TRUE(page->getSoftkeys()[1].enabled);
    Press(1);
    ASSERT_EQ(mutations.size(), 2u);
    EXPECT_NE(mutations[0].request_id, mutations[1].request_id);
}
