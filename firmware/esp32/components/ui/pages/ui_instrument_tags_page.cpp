#include "ui/ui_instrument_tags_page.h"

#include <esp_random.h>

#include "components/ui_value_tile.h"
#include "debug/console_command.h"
#include "inter_mcu.h"
#include "ui/current_track.h"
#include "ui/instrument_tags_model.h"
#include "ui/ui_navigator.h"
#include "ui_theme.h"

#include <cstdio>
#include <cstring>
namespace wavex_ui {
using namespace WaveX::Protocol;
namespace {
uint32_t nextId() {
    static uint32_t id = esp_random();
    if (!++id)
        ++id;
    return id;
}
class TagsPage final : public UIPage {
   public:
    const char* name() const override { return "Instrument Tags"; }
    void onEnter(lv_obj_t* parent) override {
        root_ = lv_obj_create(parent);
        ui_theme_apply_container_style(root_, false);
        lv_obj_set_size(root_, lv_pct(100), lv_pct(100));
        lv_obj_set_style_pad_all(root_, 0, 0);
        lv_obj_remove_flag(root_, LV_OBJ_FLAG_SCROLLABLE);
        const int width = (UI_CONTENT_WIDTH - 2 * UI_MARGIN_X - 3 * UI_GUTTER) / 4;
        for (uint8_t i = 0; i < 8; ++i) {
            tiles_[i] = valueTileCreate(
                root_,
                UI_MARGIN_X + (i % 4) * (width + UI_GUTTER),
                UI_PADDING_LARGE + (i / 4) * (UI_PERFORMANCE_TILE_HEIGHT + UI_GUTTER),
                width,
                UI_PERFORMANCE_TILE_HEIGHT,
                WaveX::InstrumentTags::kNames[i],
                "");
            valueTileSetOnAdjust(tiles_[i], [this, i](int delta) {
                focus_ = i;
                if (ready() && delta) {
                    const bool selected = model_.Draft() & WaveX::InstrumentTags::Mask(i);
                    if (selected != (delta > 0))
                        model_.Toggle(i);
                }
                render();
            });
        }
        label_ = lv_label_create(root_);
        ui_theme_apply_label_style(label_, false);
        lv_obj_set_style_text_font(label_, UI_FONT_SMALL, 0);
        lv_obj_set_pos(label_,
                       UI_MARGIN_X,
                       2 * (UI_PERFORMANCE_TILE_HEIGHT + UI_GUTTER) + 2 * UI_PADDING_LARGE);
        lv_obj_set_width(label_, UI_CONTENT_WIDTH - 2 * UI_MARGIN_X);
        lv_label_set_long_mode(label_, LV_LABEL_LONG_WRAP);
        onTrackChanged();
        timer_ = lv_timer_create(
            [](lv_timer_t* t) { static_cast<TagsPage*>(lv_timer_get_user_data(t))->service(); },
            100,
            this);
    }
    void onExit() override {
        if (timer_)
            lv_timer_delete(timer_);
        timer_ = nullptr;
        if (root_)
            lv_obj_delete(root_);
        root_ = label_ = nullptr;
        model_.Reset(getCurrentTrack());
    }
    void onTrackChanged() override {
        model_.Reset(getCurrentTrack());
        alive_ = inter_mcu_backend_link_alive();
        received_ = 0;
        message_ = "Choose categories, then Apply. Save the Instrument to keep tags on card.";
        read();
        render();
    }
    bool canLeave() override { return !model_.Pending() || !alive_; }
    void onInput(const InputEvent& e) override {
        if (e.type == InputType::EncoderLeft || e.type == InputType::EncoderRight)
            focus_ = static_cast<uint8_t>((int(focus_) + (e.steps() > 0 ? 1 : 7)) % 8);
        else if (e.type == InputType::EncoderClick && ready())
            model_.Toggle(focus_);
        render();
    }
    std::array<Softkey, NUM_SOFTKEYS> getSoftkeys() override {
        std::array<Softkey, NUM_SOFTKEYS> keys{};
        keys[0] = {"Back", [] { UINavigator::instance().pop(); }};
        keys[1] = {
            "Apply", [this] { apply(); }, ready() && model_.Dirty(), "No staged tags / reading"};
        keys[2] = {"Revert",
                   [this] {
                       model_.Revert();
                       render();
                   },
                   model_.Dirty() && !model_.Pending(),
                   "No staged tags"};
        keys[3] = {"Tag <", [this] {
                       focus_ = (focus_ + 7) % 8;
                       render();
                   }};
        keys[4] = {"Tag >", [this] {
                       focus_ = (focus_ + 1) % 8;
                       render();
                   }};
        keys[5] = {"Toggle",
                   [this] {
                       if (ready())
                           model_.Toggle(focus_);
                       render();
                   },
                   ready(),
                   "Load an Instrument / reading"};
        return keys;
    }
    size_t consoleState(char* out, size_t cap, size_t len) override {
        using namespace WaveX::Debug;
        len = AppendKvInt(out, cap, len, "tags", model_.Draft());
        len = AppendKvInt(out, cap, len, "tagsready", ready());
        return AppendKvInt(out, cap, len, "tagsdirty", model_.Dirty());
    }
    bool consoleCommand(const char* args, char* reply, size_t cap) override {
        unsigned tag;
        char extra;
        if (std::sscanf(args, "TAG %u %c", &tag, &extra) != 1 || tag >= 8 || !ready())
            return false;
        model_.Toggle(tag);
        render();
        std::snprintf(reply, cap, "OK");
        return true;
    }

   private:
    bool ready() const { return alive_ && model_.Ready() && lv_tick_get() - received_ < 1500; }
    void read() {
        if (!alive_)
            return;
        InstOpMessage q(nextId(), getCurrentTrack(), INST_OP_GET_PAD_MAP, "");
        if (inter_mcu_send_instrument_edit(q) == ESP_OK)
            model_.Expect(q.request_id);
        read_at_ = lv_tick_get();
    }
    void apply() {
        if (!ready() || !model_.Dirty())
            return;
        auto request = model_.Request(nextId());
        if (inter_mcu_send_instrument_edit(request) == ESP_OK) {
            model_.Sent(request.request_id);
            pending_at_ = lv_tick_get();
            read();
        }
        render();
    }
    void service() {
        const bool alive = inter_mcu_backend_link_alive();
        if (alive != alive_) {
            onTrackChanged();
        }
        InstZoneSyncMessage state;
        if (alive_ && inter_mcu_get_instrument_map(&state) && model_.Accept(state))
            received_ = lv_tick_get();
        if (model_.Pending() && lv_tick_get() - pending_at_ > 5000) {
            model_.Reset(getCurrentTrack());
            message_ = "Edit not confirmed; reading current tags. No automatic retry.";
            read();
        }
        if (alive_ && lv_tick_get() - read_at_ >= 300)
            read();
        render();
    }
    void render() {
        if (!root_)
            return;
        for (uint8_t i = 0; i < 8; ++i) {
            valueTileSetValue(
                tiles_[i], model_.Draft() & WaveX::InstrumentTags::Mask(i) ? "On" : "Off", true);
            valueTileSetFocus(tiles_[i], i == focus_);
        }
        const char* text = !alive_                  ? "Audio engine disconnected"
                           : !model_.State().loaded ? "Load an Instrument first."
                           : model_.Pending()       ? "Applying tags..."
                           : model_.State().error
                               ? "Edit rejected; check current Instrument and try again."
                               : message_;
        if (std::strcmp(lv_label_get_text(label_), text))
            lv_label_set_text(label_, text);
        UINavigator::instance().refreshSoftkeys();
    }
    InstrumentTagsModel model_;
    ValueTile tiles_[8]{};
    lv_obj_t* label_ = nullptr;
    lv_timer_t* timer_ = nullptr;
    uint32_t read_at_ = 0, pending_at_ = 0, received_ = 0;
    uint8_t focus_ = 0;
    bool alive_ = false;
    const char* message_ = "";
};
}  // namespace
std::shared_ptr<UIPage> createInstrumentTagsPage() {
    return std::make_shared<TagsPage>();
}
}  // namespace wavex_ui
