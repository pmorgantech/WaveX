#include <gtest/gtest.h>

#include "inter_mcu.h"
#include "ui/ui_navigator.h"
#include "ui/ui_sequencer_page.h"
#include "ui_theme.h"

#include <cstring>
using namespace WaveX::Protocol;
namespace {
uint16_t pixels[1280 * 800];
uint32_t ticks = 0, submitted = 0, full = 0;
SeqPatternRequestMessage read_request;
SeqPlayheadMessage playhead;
uint32_t Tick() {
    return ticks;
}
void Flush(lv_display_t* display, const lv_area_t* area, uint8_t*) {
    const auto size = lv_area_get_size(area);
    submitted += size;
    if (size == 1280 * 800)
        ++full;
    lv_display_flush_ready(display);
}
lv_obj_t* FindLabel(lv_obj_t* root, const char* text) {
    if (lv_obj_check_type(root, &lv_label_class) && !std::strcmp(lv_label_get_text(root), text))
        return root;
    for (uint32_t i = 0; i < lv_obj_get_child_count(root); ++i)
        if (auto* found = FindLabel(lv_obj_get_child(root, i), text))
            return found;
    return nullptr;
}
class EmptyPage : public wavex_ui::UIPage {
   public:
    const char* name() const override { return "Root"; }
    void onEnter(lv_obj_t*) override {}
};
class SequencerPageTest : public ::testing::Test {
   protected:
    std::shared_ptr<wavex_ui::UIPage> page;
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
        playhead = {};
        page = wavex_ui::createSequencerPage();
        wavex_ui::UINavigator::instance().push(page);
        Advance(30);
        submitted = full = 0;
    }
    void TearDown() override { wavex_ui::UINavigator::instance().pop(); }
    void Advance(unsigned n) {
        while (n--) {
            ticks += 100;
            lv_timer_handler();
        }
    }
    void Command(const char* cmd) {
        char reply[128];
        ASSERT_TRUE(page->consoleCommand(cmd, reply, sizeof(reply)));
    }
};
}  // namespace
namespace wavex_ui {
void statusStripCreate(lv_obj_t*) {}
std::shared_ptr<UIPage> createNotesPage(uint8_t) {
    return std::make_shared<EmptyPage>();
}
std::shared_ptr<UIPage> createPatternSlotsPage() {
    return std::make_shared<EmptyPage>();
}
}  // namespace wavex_ui
bool inter_mcu_backend_link_alive() {
    return true;
}
bool inter_mcu_get_track_binding(uint8_t, TrackBindingMessage*) {
    return false;
}
esp_err_t inter_mcu_request_track_binding(uint8_t) {
    return ESP_OK;
}
esp_err_t inter_mcu_send_project_op(const ProjectOpMessage&) {
    return ESP_OK;
}
esp_err_t inter_mcu_send_mix_op(uint8_t, uint8_t, uint16_t) {
    return ESP_OK;
}
esp_err_t inter_mcu_request_seq_slot_page(const SeqPatternRequestMessage& r) {
    read_request = r;
    return ESP_OK;
}
bool inter_mcu_get_seq_slot_page(SeqSlotPageMessage* out) {
    *out = {};
    out->epoch = 1;
    auto& p = out->page;
    p.request_id = read_request.request_id;
    p.track = read_request.track;
    p.first_step = read_request.first_step;
    p.valid = p.enabled = 1;
    p.length = 32;
    p.tempo_bpm_x100 = 12000;
    p.scale = 1;
    for (auto& s: p.steps) {
        s.on = 1;
        s.velocity = 100;
        s.probability = 100;
        s.note = 60;
    }
    return true;
}
bool inter_mcu_get_seq_playhead(SeqPlayheadMessage* out) {
    *out = playhead;
    return true;
}
esp_err_t inter_mcu_send_seq_transport(const SeqTransportMessage&) {
    return ESP_OK;
}
esp_err_t inter_mcu_send_seq_slot_edit(const SeqSlotEditMessage&) {
    return ESP_OK;
}
TEST_F(SequencerPageTest, LocksModeChangesStayPartialAndKeepTheGridAlive) {
    auto* grid = FindLabel(lv_screen_active(), "01");
    ASSERT_NE(grid, nullptr);
    Command("HOLD 1");
    Advance(1);
    EXPECT_EQ(full, 0u);
    EXPECT_GT(submitted, 0u);
    EXPECT_LT(submitted, 1280u * 800u);
    EXPECT_TRUE(lv_obj_is_valid(grid));
    RecordProperty("locks_pixels", submitted);
    submitted = full = 0;
    Command("HOLD 0");
    auto keys = page->getSoftkeys();
    ASSERT_TRUE(keys[1].onPress);
    keys[1].onPress();
    Advance(1);
    EXPECT_EQ(full, 0u);
    EXPECT_GT(submitted, 0u);
    EXPECT_LT(submitted, 1280u * 800u);
    RecordProperty("return_pixels", submitted);
    submitted = full = 0;
    Advance(10);
    EXPECT_EQ(submitted, 0u);
}
