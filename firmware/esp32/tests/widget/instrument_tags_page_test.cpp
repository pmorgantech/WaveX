#include <gtest/gtest.h>

#include "inter_mcu.h"
#include "ui/ui_instrument_tags_page.h"
#include "ui/ui_navigator.h"

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
InstZoneSyncMessage status;
std::vector<InstOpMessage> mutations;
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
class InstrumentTagsPageTest : public ::testing::Test {
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
        status.loaded = 1;
        status.revision = 7;
        mutations.clear();
        page = wavex_ui::createInstrumentTagsPage();
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
esp_err_t inter_mcu_send_instrument_edit(const InstOpMessage& request) {
    read_id = request.request_id;
    if (request.op != INST_OP_GET_PAD_MAP) {
        mutations.push_back(request);
        status.busy = 1;
    }
    return ESP_OK;
}
bool inter_mcu_get_instrument_map(InstZoneSyncMessage* out) {
    if (!respond)
        return false;
    *out = status;
    out->request_id = read_id;
    return true;
}

TEST_F(InstrumentTagsPageTest, MultipleCategoriesWaitForExplicitApplyAndConfirmedReadback) {
    char reply[32];
    ASSERT_TRUE(page->consoleCommand("TAG 0", reply, sizeof(reply)));
    ASSERT_TRUE(page->consoleCommand("TAG 7", reply, sizeof(reply)));
    EXPECT_TRUE(mutations.empty());
    Press(1);
    ASSERT_EQ(mutations.size(), 1u);
    EXPECT_EQ(mutations[0].tags, 0x81);
    EXPECT_EQ(mutations[0].revision, 7u);
    EXPECT_FALSE(page->getSoftkeys()[1].enabled);
    status.busy = 0;
    status.tags = 0x81;
    status.revision = 8;
    status.completed_request_id = mutations[0].request_id;
    Advance(5);
    EXPECT_FALSE(page->getSoftkeys()[1].enabled);
    ASSERT_TRUE(page->consoleCommand("TAG 2", reply, sizeof(reply)));
    Press(2);
    EXPECT_EQ(mutations.size(), 1u);
    EXPECT_FALSE(page->getSoftkeys()[1].enabled);
}
TEST_F(InstrumentTagsPageTest, StaleAndDisconnectedStateCannotAuthorizeEdits) {
    respond = false;
    Advance(20);
    char reply[32];
    EXPECT_FALSE(page->consoleCommand("TAG 0", reply, sizeof(reply)));
    alive = false;
    Advance(3);
    EXPECT_FALSE(page->getSoftkeys()[5].enabled);
    EXPECT_TRUE(mutations.empty());
}
