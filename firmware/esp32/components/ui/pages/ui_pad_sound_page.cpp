#include "ui/ui_pad_sound_page.h"

#include <esp_random.h>

#include "../styles/ui_theme.h"
#include "debug/console_command.h"
#include "inter_mcu.h"
#include "ui/current_track.h"
#include "ui/ui_navigator.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace wavex_ui {
using namespace WaveX::Protocol;
namespace {
uint32_t nextId() {
    static uint32_t id = esp_random();
    if (++id == 0)
        ++id;
    return id;
}
constexpr const char* names[] = {"CUTOFF", "ATTACK", "DECAY", "SUSTAIN"};
constexpr int maxima[] = {20000, 10000, 10000, 1000};
void text(lv_obj_t* object, const char* value) {
    if (object && std::strcmp(lv_label_get_text(object), value))
        lv_label_set_text(object, value);
}
}  // namespace
void UIPadSoundPage::onEnter(lv_obj_t* parent) {
    root_ = lv_obj_create(parent);
    ui_theme_apply_container_style(root_, false);
    lv_obj_set_size(root_, UI_CONTENT_WIDTH, UI_CONTENT_HEIGHT);
    lv_obj_set_style_pad_all(root_, 0, 0);
    lv_obj_remove_flag(root_, LV_OBJ_FLAG_SCROLLABLE);
    const int width = (UI_CONTENT_WIDTH - 2 * UI_MARGIN_X - 3 * UI_GUTTER) / 4;
    const int height = UI_CONTENT_HEIGHT / 2;
    for (uint8_t i = 0; i < 4; ++i) {
        tiles_[i] = valueTileCreate(root_,
                                    UI_MARGIN_X + i * (width + UI_GUTTER),
                                    UI_PADDING_LARGE,
                                    width,
                                    height,
                                    names[i],
                                    i == 0 ? "Hz" : (i == 3 ? "%" : "ms"));
        valueTileSetOnAdjust(tiles_[i], [this, i](int delta) { adjust(i, delta); });
    }
    status_ = lv_label_create(root_);
    ui_theme_apply_label_style(status_, false);
    lv_obj_set_style_text_font(status_, UI_FONT_SMALL, 0);
    lv_obj_set_pos(status_, UI_MARGIN_X, height + 2 * UI_PADDING_LARGE);
    lv_obj_set_width(status_, UI_CONTENT_WIDTH - 2 * UI_MARGIN_X);
    lv_label_set_long_mode(status_, LV_LABEL_LONG_WRAP);
    timer_ = lv_timer_create(tick, 100, this);
    onTrackChanged();
}
void UIPadSoundPage::onExit() {
    if (timer_)
        lv_timer_delete(timer_);
    timer_ = nullptr;
    if (root_)
        lv_obj_delete(root_);
    root_ = status_ = nullptr;
    for (auto& tile: tiles_)
        tile = ValueTile{};
    model_.Reset(getCurrentTrack(), pad_);
}
void UIPadSoundPage::onTrackChanged() {
    model_.Reset(getCurrentTrack(), pad_);
    softkey_state_ = UINT32_MAX;
    timed_out_ = false;
    pending_at_ = 0;
    alive_ = inter_mcu_backend_link_alive();
    read();
    render();
}
void UIPadSoundPage::read() {
    if (!alive_)
        return;
    InstPadSoundOpMessage request{nextId(), getCurrentTrack(), pad_, PAD_SOUND_GET, 0, 0};
    if (inter_mcu_send_pad_sound(request) == ESP_OK)
        model_.Expect(request.request_id);
    read_at_ = lv_tick_get();
}
void UIPadSoundPage::service() {
    const bool alive = inter_mcu_backend_link_alive();
    if (alive != alive_) {
        alive_ = alive;
        model_.Reset(getCurrentTrack(), pad_);
        pending_at_ = 0;
        if (alive)
            read();
    }
    InstPadSoundSyncMessage received;
    const auto completed = model_.Snapshot().completed_request_id;
    if (alive_ && inter_mcu_get_pad_sound(&received) && model_.Accept(received) &&
        model_.Pending() && received.completed_request_id != completed)
        pending_at_ = lv_tick_get();
    const auto now = lv_tick_get();
    if (!model_.Pending())
        pending_at_ = 0;
    if (pending_at_ && static_cast<uint32_t>(now - pending_at_) > 5000) {
        model_.Reset(getCurrentTrack(), pad_);
        pending_at_ = 0;
        timed_out_ = true;
        read();
    }
    InstPadSoundOpMessage request;
    if (alive_ && model_.Next(nextId(), request)) {
        if (inter_mcu_send_pad_sound(request) != ESP_OK)
            model_.SendFailed();
        else
            read_at_ = now;
    }
    if (alive_ && static_cast<uint32_t>(now - read_at_) >= 300)
        read();
    render();
}
void UIPadSoundPage::render() {
    if (!root_)
        return;
    char value[64];
    for (uint8_t i = 0; i < 4; ++i) {
        const auto amount = model_.Value(i);
        if (!model_.Valid() || !model_.Snapshot().valid)
            std::snprintf(value, sizeof(value), "--");
        else if (i == 3)
            std::snprintf(value, sizeof(value), "%u.%u", amount / 10, amount % 10);
        else
            std::snprintf(value, sizeof(value), "%u", amount);
        if (std::strcmp(lv_label_get_text(tiles_[i].value), value)) {
            valueTileSetValue(tiles_[i], value);
            valueTileSetFill(tiles_[i], static_cast<float>(amount) / maxima[i]);
        }
    }
    const char* message = !alive_                    ? "Audio engine disconnected"
                          : !model_.Valid()          ? "Reading pad..."
                          : !model_.Snapshot().valid ? "Assign a sample to this pad in Pad Map."
                          : model_.Snapshot().busy   ? "Instrument loader busy."
                          : model_.Pending()         ? "Applying pad sound..."
                          : timed_out_ ? "Edit not confirmed. Values shown follow the audio engine."
                          : model_.Error()        ? "Edit rejected. Check the pad and try again."
                          : model_.Snapshot().own ? "Pad override"
                                                  : "Using Instrument settings";
    char status[320];
    std::snprintf(status,
                  sizeof(status),
                  "%s\n\nDrag a value to edit this pad. Changes apply to subsequent hits.\n"
                  "Resonance follows the Instrument. One-shot pads ignore note-off release.\n"
                  "Inherit restores all inherited settings. Save the kit from Pad Map.",
                  message);
    text(status_, status);
    char context[96];
    std::snprintf(context,
                  sizeof(context),
                  "Track %u / Pad %u",
                  trackDisplayNumber(getCurrentTrack()),
                  pad_ + 1);
    if (std::strcmp(context_, context)) {
        std::snprintf(context_, sizeof(context_), "%s", context);
        UINavigator::instance().refreshContext();
    }
    const uint32_t keys =
        static_cast<uint32_t>(alive_) | (static_cast<uint32_t>(model_.Ready()) << 1) |
        (static_cast<uint32_t>(model_.Pending()) << 2) |
        (static_cast<uint32_t>(model_.Snapshot().valid) << 3) |
        (static_cast<uint32_t>(model_.Snapshot().own) << 4) | (static_cast<uint32_t>(pad_) << 8);
    if (keys != softkey_state_) {
        softkey_state_ = keys;
        UINavigator::instance().refreshSoftkeys();
    }
}
void UIPadSoundPage::adjust(uint8_t field, int delta) {
    if (!alive_ || field >= 4 || !model_.Editable())
        return;
    const int step = field == 0 ? (model_.Value(field) < 1000 ? 10 : 100) : (field == 3 ? 10 : 1);
    const auto value = static_cast<uint16_t>(
        std::clamp(static_cast<int64_t>(model_.Value(field)) + static_cast<int64_t>(delta) * step,
                   static_cast<int64_t>(field == 0 ? 20 : 0),
                   static_cast<int64_t>(maxima[field])));
    if (model_.Set(field, value)) {
        if (!pending_at_)
            pending_at_ = lv_tick_get();
        timed_out_ = false;
        service();
    }
}
void UIPadSoundPage::select(int delta) {
    if (model_.Pending())
        return;
    pad_ = static_cast<uint8_t>(std::clamp(static_cast<int>(pad_) + delta, 0, 15));
    onTrackChanged();
}
std::array<Softkey, NUM_SOFTKEYS> UIPadSoundPage::getSoftkeys() {
    std::array<Softkey, NUM_SOFTKEYS> keys{};
    const bool ready = alive_ && model_.Ready();
    keys[0] = {"Back", [] { UINavigator::instance().pop(); }, !model_.Pending(), "Applying edit"};
    keys[1] = {"Audition",
               [this] {
                   inter_mcu_send_note_on_track(INST_PAD_FIRST_NOTE + pad_, 100, getCurrentTrack());
               },
               ready && model_.Snapshot().valid,
               "Assign a sample first"};
    keys[2] = {"Inherit",
               [this] {
                   if (model_.Inherit()) {
                       pending_at_ = lv_tick_get();
                       timed_out_ = false;
                       service();
                   }
               },
               ready && model_.Snapshot().valid && model_.Snapshot().own,
               "Already inherited"};
    keys[4] = {"Pad -", [this] { select(-1); }, !model_.Pending() && pad_ > 0, "First pad"};
    keys[5] = {"Pad +", [this] { select(1); }, !model_.Pending() && pad_ < 15, "Last pad"};
    return keys;
}
size_t UIPadSoundPage::consoleState(char* out, size_t cap, size_t len) {
    using namespace WaveX::Debug;
    len = AppendKvInt(out, cap, len, "soundready", alive_ && model_.Ready());
    len = AppendKvInt(out, cap, len, "soundvalid", model_.Valid() && model_.Snapshot().valid);
    len = AppendKvInt(out, cap, len, "soundpad", pad_ + 1);
    len = AppendKvInt(out, cap, len, "soundown", model_.Snapshot().own);
    len = AppendKvInt(out, cap, len, "sounderror", model_.Error());
    len = AppendKvInt(out, cap, len, "cutoff", model_.Value(0));
    len = AppendKvInt(out, cap, len, "attack", model_.Value(1));
    len = AppendKvInt(out, cap, len, "decay", model_.Value(2));
    return AppendKvInt(out, cap, len, "sustain", model_.Value(3));
}
bool UIPadSoundPage::consoleCommand(const char* args, char* reply, size_t cap) {
    char name[16];
    int value;
    char extra;
    if (!args || std::sscanf(args, "%15s %d %c", name, &value, &extra) != 2 || value < 0 ||
        value > 65535 || !alive_)
        return false;
    for (uint8_t i = 0; i < 4; ++i)
        if (!std::strcmp(name, names[i])) {
            if (!model_.Set(i, static_cast<uint16_t>(value)))
                return false;
            if (!pending_at_)
                pending_at_ = lv_tick_get();
            timed_out_ = false;
            service();
            std::snprintf(reply, cap, "ok");
            return true;
        }
    return false;
}
void UIPadSoundPage::tick(lv_timer_t* t) {
    static_cast<UIPadSoundPage*>(lv_timer_get_user_data(t))->service();
}
std::shared_ptr<UIPage> createPadSoundPage(uint8_t pad) {
    return std::make_shared<UIPadSoundPage>(pad);
}
}  // namespace wavex_ui
