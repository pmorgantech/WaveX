#include <gtest/gtest.h>

#include "inter_mcu.h"
#include "ui/current_track.h"
#include "ui/ui_bank_page.h"
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
uint16_t pixels[1280 * 720];
uint32_t ticks = 0, flushes = 0;
bool alive = true, respond = true;
BankOpMessage query;
BankStatusMessage status;
std::vector<BankOpMessage> mutations;
uint32_t Tick() {
    return ticks;
}
void Flush(lv_display_t* d, const lv_area_t*, uint8_t*) {
    ++flushes;
    lv_display_flush_ready(d);
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
class BankPageTest : public ::testing::Test {
   protected:
    void SetUp() override {
        static bool initialized = false;
        if (!initialized) {
            lv_init();
            lv_tick_set_cb(Tick);
            auto* d = lv_display_create(1280, 720);
            lv_display_set_buffers(
                d, pixels, nullptr, sizeof(pixels), LV_DISPLAY_RENDER_MODE_DIRECT);
            lv_display_set_flush_cb(d, Flush);
            wavex_ui::UINavigator::instance().push(std::make_shared<Root>());
            initialized = true;
        }
        alive = respond = true;
        status = {};
        status.loaded = status.occupied = 1;
        std::strcpy(status.name, "Studio kit");
        std::strcpy(status.instrument, "Drums");
        mutations.clear();
        query = {};
        wavex_ui::setCurrentTrack(3);
        page = std::make_shared<wavex_ui::UIBankPage>();
        wavex_ui::UINavigator::instance().push(page);
        Advance(5);
    }
    void TearDown() override { wavex_ui::UINavigator::instance().pop(); }
    void Advance(unsigned n) {
        for (unsigned i = 0; i < n; ++i) {
            ticks += 100;
            lv_timer_handler();
        }
    }
    void Press(unsigned i, bool shifted = false) {
        auto keys = shifted ? page->getShiftedSoftkeys() : page->getSoftkeys();
        ASSERT_TRUE(keys[i].enabled);
        ASSERT_TRUE(keys[i].onPress);
        keys[i].onPress();
        Advance(1);
    }
    std::shared_ptr<wavex_ui::UIBankPage> page;
};
}  // namespace
bool inter_mcu_backend_link_alive() {
    return alive;
}
esp_err_t inter_mcu_send_bank_op(const BankOpMessage& request) {
    query = request;
    if (request.op != BANK_GET) {
        mutations.push_back(request);
        status.busy = 1;
        status.active_request_id = request.request_id;
        status.active_op = request.op;
    }
    return ESP_OK;
}
bool inter_mcu_get_bank_status(BankStatusMessage* out) {
    if (!respond)
        return false;
    *out = status;
    out->request_id = query.request_id;
    out->slot = query.slot;
    return true;
}
TEST_F(BankPageTest, RecallRequiresConfirmationAndCapturesSelectedTrack) {
    Press(3);
    EXPECT_TRUE(mutations.empty());
    Press(0);
    EXPECT_TRUE(mutations.empty());
    Press(3);
    Press(1);
    ASSERT_EQ(mutations.size(), 1u);
    EXPECT_EQ(mutations[0].op, BANK_RECALL);
    EXPECT_EQ(mutations[0].track, 3);
    EXPECT_EQ(mutations[0].flags, BANK_CONFIRM_REPLACE);
    Advance(30);
    EXPECT_EQ(mutations.size(), 1u);
}
TEST_F(BankPageTest, ChangingTrackOrBankCancelsConfirmation) {
    Press(3);
    wavex_ui::setCurrentTrack(4);
    page->onTrackChanged();
    EXPECT_EQ(page->getSoftkeys()[1].label, "Previous");
    EXPECT_TRUE(mutations.empty());
    Press(3);
    ++status.revision;
    Advance(5);
    EXPECT_EQ(page->getSoftkeys()[1].label, "Previous");
    EXPECT_TRUE(mutations.empty());
}
TEST_F(BankPageTest, StoreUsesNewNameAndStableSlotAndDisconnectNeverReplays) {
    Press(1);
    Advance(4);  // wrap to slot 128
    char reply[32];
    ASSERT_TRUE(page->consoleCommand("NAME Night kit", reply, sizeof(reply)));
    Press(4);
    Press(1);
    ASSERT_EQ(mutations.size(), 1u);
    EXPECT_EQ(mutations[0].slot, 127);
    EXPECT_EQ(mutations[0].op, BANK_STORE_COPY);
    EXPECT_STREQ(mutations[0].name, "Night kit");
    alive = false;
    Advance(3);
    alive = true;
    Advance(20);
    EXPECT_EQ(mutations.size(), 1u);
}
TEST_F(BankPageTest, StaleEmptyAndBusySnapshotsDisableRecall) {
    status.occupied = 0;
    Advance(5);
    EXPECT_FALSE(page->getSoftkeys()[3].enabled);
    status.occupied = 1;
    status.blocked = 1;
    Advance(5);
    EXPECT_FALSE(page->getSoftkeys()[3].enabled);
    status.blocked = 0;
    Advance(5);
    EXPECT_TRUE(page->getSoftkeys()[3].enabled);
    respond = false;
    Advance(20);
    EXPECT_FALSE(page->getSoftkeys()[3].enabled);
}
TEST_F(BankPageTest, RenderLayoutAndIdleRefresh) {
    lv_refr_now(nullptr);
    if (const char* path = std::getenv("WAVEX_BANK_PREVIEW")) {
        FILE* out = std::fopen(path, "wb");
        ASSERT_NE(out, nullptr);
        std::fprintf(out, "P6\n1280 720\n255\n");
        for (uint16_t p: pixels) {
            unsigned char rgb[] = {static_cast<unsigned char>(((p >> 11) & 31) * 255 / 31),
                                   static_cast<unsigned char>(((p >> 5) & 63) * 255 / 63),
                                   static_cast<unsigned char>((p & 31) * 255 / 31)};
            std::fwrite(rgb, 1, 3, out);
        }
        std::fclose(out);
    }
    Advance(5);
    const auto settled = flushes;
    Advance(50);
    EXPECT_EQ(flushes, settled);
    EXPECT_TRUE(mutations.empty());
    EXPECT_TRUE(page->getSoftkeys()[3].enabled);
}

TEST_F(BankPageTest, PreloadUsesActiveBankWithoutNameOrReplacementConfirmation) {
    status.occupied = 0;  // selected slot need not be occupied
    Advance(5);
    Press(5);
    ASSERT_EQ(mutations.size(), 1u);
    EXPECT_EQ(mutations[0].op, BANK_PRELOAD);
    EXPECT_EQ(mutations[0].flags, 0);
    EXPECT_STREQ(mutations[0].name, "");
    EXPECT_FALSE(page->getSoftkeys()[5].enabled);
    alive = false;
    Advance(3);
    alive = true;
    Advance(20);
    EXPECT_EQ(mutations.size(), 1u);
}
TEST_F(BankPageTest, PreloadNeedsLoadedFreshBank) {
    status.loaded = status.occupied = 0;
    Advance(5);
    EXPECT_FALSE(page->getSoftkeys()[5].enabled);
    status.loaded = 1;
    Advance(5);
    EXPECT_TRUE(page->getSoftkeys()[5].enabled);
    respond = false;
    Advance(20);
    EXPECT_FALSE(page->getSoftkeys()[5].enabled);
}
