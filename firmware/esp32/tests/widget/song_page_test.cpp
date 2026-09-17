#include <gtest/gtest.h>

#include "inter_mcu.h"
#include "ui/mixer_solo.h"
#include "ui/ui_navigator.h"
#include "ui/ui_song_page.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
using namespace WaveX::Protocol;
namespace wavex_ui {
void statusStripCreate(lv_obj_t*) {}
std::shared_ptr<UIPage> createPatternSlotsPage() {
    return {};
}
std::shared_ptr<UIPage> createProjectFilesPage() {
    return {};
}
}  // namespace wavex_ui
namespace {
uint16_t pixels[1280 * 720];
uint32_t ticks = 0, read_id = 0, flushes = 0;
bool alive = true, respond = true;
SeqSongStatusMessage status;
std::vector<SeqSongOpMessage> mutations;
uint32_t Tick() {
    return ticks;
}
void Flush(lv_display_t* display, const lv_area_t*, uint8_t*) {
    ++flushes;
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
class SongPageTest : public ::testing::Test {
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
        page = std::make_shared<wavex_ui::UISongPage>();
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
    std::shared_ptr<wavex_ui::UISongPage> page;
};
}  // namespace
bool inter_mcu_backend_link_alive() {
    return alive;
}
esp_err_t inter_mcu_send_seq_song_op(const SeqSongOpMessage& request) {
    read_id = request.request_id;
    status.song = request.song;
    if (request.op != SEQ_SONG_GET) {
        mutations.push_back(request);
        status.busy = 1;
        status.active_request_id = request.request_id;
    }
    return ESP_OK;
}
bool inter_mcu_get_seq_song_status(SeqSongStatusMessage* out) {
    if (!respond)
        return false;
    *out = status;
    out->request_id = read_id;
    return true;
}

TEST_F(SongPageTest, CreateAndPlaybackWaitForMatchingResults) {
    Name();
    Press(1);
    ASSERT_EQ(mutations.size(), 1u);
    EXPECT_EQ(mutations[0].op, SEQ_SONG_CREATE);
    EXPECT_STREQ(mutations[0].name, "Night");
    EXPECT_FALSE(page->getSoftkeys()[1].enabled);
    status.busy = 0;
    status.active_request_id = 0;
    status.completed_request_id = mutations[0].request_id;
    status.completed_op = SEQ_SONG_CREATE;
    status.used = 1;
    status.length = 1;
    std::strcpy(status.name, "Night");
    Advance(5);
    EXPECT_TRUE(page->getSoftkeys()[4].enabled);
    Press(4);
    ASSERT_EQ(mutations.size(), 2u);
    EXPECT_EQ(mutations[1].op, SEQ_SONG_PLAY);
    status.completed_request_id = mutations[1].request_id;
    status.completed_op = SEQ_SONG_PLAY;
    status.playing_song = 0;
    status.playing_repeat = 1;
    Advance(200);
    EXPECT_EQ(mutations.size(), 2u);
    EXPECT_FALSE(page->getSoftkeys()[2].enabled);
    EXPECT_STREQ(page->getSoftkeys()[4].label.c_str(), "Stop");
    Press(4);
    ASSERT_EQ(mutations.size(), 3u);
    EXPECT_EQ(mutations[2].op, SEQ_SONG_STOP);
}
TEST_F(SongPageTest, ArrangeReferenceRepeatsAndReorderWithStagedEdits) {
    status.used = 1;
    status.length = 2;
    std::strcpy(status.name, "Night");
    Advance(5);
    char reply[64];
    ASSERT_TRUE(page->consoleCommand("FIELD 1", reply, sizeof(reply)));
    page->onInput({wavex_ui::InputType::EncoderRight});
    EXPECT_TRUE(page->getSoftkeys()[1].enabled);
    EXPECT_FALSE(page->getSoftkeys()[4].enabled);
    Press(1);
    ASSERT_EQ(mutations.size(), 1u);
    EXPECT_EQ(mutations[0].op, SEQ_SONG_SET_ENTRY);
    EXPECT_EQ(mutations[0].pattern, 1);
    EXPECT_EQ(mutations[0].entry, 0);
    status.busy = 0;
    status.active_request_id = 0;
    status.completed_request_id = mutations[0].request_id;
    status.completed_op = SEQ_SONG_SET_ENTRY;
    status.entries[0].pattern = 1;
    Advance(5);
    const auto shifted = page->getShiftedSoftkeys();
    ASSERT_TRUE(shifted[3].enabled);
    shifted[3].onPress();
    ASSERT_EQ(mutations.size(), 2u);
    EXPECT_EQ(mutations[1].op, SEQ_SONG_MOVE);
    EXPECT_EQ(mutations[1].destination, 1);
}
TEST_F(SongPageTest, StaleReadbackAndReentryDoNotPermitEdits) {
    status.used = 1;
    status.length = 1;
    Advance(5);
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
TEST_F(SongPageTest, Preview) {
    status.used = 1;
    status.length = 8;
    std::strcpy(status.name, "Night drive");
    for (uint8_t i = 0; i < status.length; ++i)
        status.entries[i] = {static_cast<uint8_t>(i % 3), static_cast<uint8_t>(i + 1)};
    status.playing_song = 0;
    status.playing_entry = 1;
    status.playing_repeat = 2;
    status.busy = 1;
    status.active_request_id = 99;
    Advance(5);
    lv_refr_now(nullptr);
    if (const char* path = std::getenv("WAVEX_SONG_PREVIEW")) {
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

TEST_F(SongPageTest, IdenticalStatusPollingDoesNotRepaint) {
    status.used = 1;
    status.length = 1;
    std::strcpy(status.name, "Steady");
    Advance(20);
    const auto before = flushes;
    Advance(30);
    EXPECT_EQ(flushes, before);
}
