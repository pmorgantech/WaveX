#include "ui/ui_allocation_page.h"

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
namespace A = WaveX::Allocation;
namespace {
uint32_t nextId() {
    static uint32_t id = esp_random();
    if (++id == 0)
        ++id;
    return id;
}
constexpr const char* names[] = {"SETTINGS", "PLAY MODE", "GROUP LIMIT", "STEAL FROM"};
void text(lv_obj_t* object, const char* value) {
    if (object && std::strcmp(lv_label_get_text(object), value))
        lv_label_set_text(object, value);
}
}  // namespace
void UIAllocationPage::onEnter(lv_obj_t* parent) {
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
                                    "");
        valueTileSetOnAdjust(tiles_[i], [this, i](int d) {
            focus_ = i;
            adjust(i, d);
        });
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
void UIAllocationPage::onExit() {
    if (timer_)
        lv_timer_delete(timer_);
    timer_ = nullptr;
    if (root_)
        lv_obj_delete(root_);
    root_ = status_ = nullptr;
    for (auto& t: tiles_)
        t = {};
    model_.Reset(getCurrentTrack(), scope_);
}
void UIAllocationPage::onTrackChanged() {
    model_.Reset(getCurrentTrack(), scope_);
    pending_at_ = 0;
    timed_out_ = false;
    keys_ = UINT32_MAX;
    alive_ = inter_mcu_backend_link_alive();
    read();
    render();
}
void UIAllocationPage::read() {
    if (!alive_)
        return;
    AllocationOpMessage request;
    request.request_id = nextId();
    request.track = getCurrentTrack();
    request.scope = scope_;
    if (inter_mcu_send_allocation(request) == ESP_OK)
        model_.Expect(request.request_id);
    read_at_ = lv_tick_get();
}
void UIAllocationPage::service() {
    const bool alive = inter_mcu_backend_link_alive();
    if (alive != alive_) {
        alive_ = alive;
        model_.Reset(getCurrentTrack(), scope_);
        pending_at_ = 0;
        if (alive)
            read();
    }
    AllocationSyncMessage state;
    if (alive_ && inter_mcu_get_allocation(&state))
        model_.Accept(state);
    const auto now = lv_tick_get();
    if (!model_.Pending())
        pending_at_ = 0;
    if (model_.Pending() && static_cast<uint32_t>(now - pending_at_) > 5000) {
        model_.Reset(getCurrentTrack(), scope_);
        pending_at_ = 0;
        timed_out_ = true;
        read();
    }
    if (alive_ && static_cast<uint32_t>(now - read_at_) >= 300)
        read();
    render();
}
bool UIAllocationPage::send(uint8_t op, A::Policy policy, bool inherit) {
    AllocationOpMessage request;
    if (!alive_ || !model_.Begin(nextId(), op, policy, inherit, request))
        return false;
    if (inter_mcu_send_allocation(request) != ESP_OK) {
        model_.SendFailed();
        timed_out_ = true;
        render();
        return false;
    }
    pending_at_ = lv_tick_get();
    timed_out_ = false;
    render();
    return true;
}
void UIAllocationPage::adjust(uint8_t field, int delta) {
    if (!delta || !alive_ || !model_.Ready())
        return;
    auto policy = model_.Effective();
    bool inherit = scope_ == ALLOC_TRACK && model_.State().inherited;
    if (field == 0) {
        if (scope_ != ALLOC_TRACK)
            return;
        inherit = delta < 0;
        if (inherit == static_cast<bool>(model_.State().inherited))
            return;
    } else {
        inherit = false;
        if (field == 1)
            policy.mode =
                static_cast<A::PlayMode>(std::clamp(static_cast<int>(policy.mode) + delta, 0, 1));
        else if (field == 2)
            policy.limit =
                static_cast<uint8_t>(std::clamp(static_cast<int>(policy.limit) + delta, 0, 8));
        else if (field == 3)
            policy.steal =
                static_cast<A::StealFrom>(std::clamp(static_cast<int>(policy.steal) + delta, 0, 2));
        else
            return;
    }
    send(ALLOC_SET, policy, inherit);
}
void UIAllocationPage::render() {
    if (!root_)
        return;
    const auto& s = model_.State();
    const auto p = model_.Effective();
    const char* steal[] = {"Own only", "Own first", "Any"};
    char limit[16];
    std::snprintf(limit, sizeof(limit), "%u", p.limit);
    const char* values[] = {
        scope_ == ALLOC_SOUND ? "Instrument" : (s.inherited ? "Instrument" : "Track"),
        p.mode == A::PlayMode::Mono ? "Mono" : "Poly",
        p.limit ? limit : "Auto",
        steal[static_cast<uint8_t>(p.steal)]};
    for (uint8_t i = 0; i < 4; ++i) {
        valueTileSetFocus(tiles_[i], i == focus_ && alive_ && model_.Ready());
        const char* value = model_.Valid() && s.valid ? values[i] : "--";
        if (std::strcmp(lv_label_get_text(tiles_[i].value), value))
            valueTileSetValue(tiles_[i], value);
    }
    const char* message = !alive_            ? "Audio engine disconnected"
                          : !model_.Valid()  ? "Reading polyphony..."
                          : !s.valid         ? "Load an Instrument first."
                          : s.busy           ? "Instrument loader busy."
                          : model_.Pending() ? "Applying polyphony..."
                          : timed_out_       ? "Edit not confirmed. Showing audio engine settings."
                          : s.error          ? "Edit rejected. Try again."
                          : s.dirty ? "Edited. Apply keeps changes; Revert restores the edit point."
                                    : "Settings applied.";
    char status[440];
    std::snprintf(
        status,
        sizeof(status),
        "%s\n\nMono: one group; releasing its key retriggers the previous held key.\n"
        "Group limit includes release tails. Changes affect new notes.\n%s",
        message,
        scope_ == ALLOC_SOUND
            ? "Save the Instrument or kit to keep these settings on the card."
            : "Track overrides are saved with the Project. Instrument uses the sound's settings.");
    text(status_, status);
    const uint32_t state =
        static_cast<uint32_t>(alive_) | (static_cast<uint32_t>(model_.Ready()) << 1) |
        (static_cast<uint32_t>(model_.Pending()) << 2) | (static_cast<uint32_t>(s.dirty) << 3) |
        (static_cast<uint32_t>(focus_) << 8);
    if (state != keys_) {
        keys_ = state;
        UINavigator::instance().refreshSoftkeys();
    }
}
std::array<Softkey, NUM_SOFTKEYS> UIAllocationPage::getSoftkeys() {
    std::array<Softkey, NUM_SOFTKEYS> keys{};
    const bool ready = alive_ && model_.Ready();
    keys[0] = {"Back", [] { UINavigator::instance().pop(); }, !model_.Pending(), "Applying edit"};
    keys[1] = {
        "Apply",
        [this] { send(ALLOC_APPLY, model_.Effective(), scope_ && model_.State().inherited); },
        ready && model_.State().dirty,
        "No edits"};
    keys[2] = {
        "Revert",
        [this] { send(ALLOC_REVERT, model_.Effective(), scope_ && model_.State().inherited); },
        ready && model_.State().dirty,
        "No edits"};
    keys[3] = {"Field >", [this] {
                   focus_ = static_cast<uint8_t>((focus_ + 1) % 4);
                   if (!scope_ && !focus_)
                       focus_ = 1;
                   render();
               }};
    keys[4] = {"Value -", [this] { adjust(focus_, -1); }, ready, "Reading settings"};
    keys[5] = {"Value +", [this] { adjust(focus_, 1); }, ready, "Reading settings"};
    return keys;
}
void UIAllocationPage::onInput(const InputEvent& e) {
    if (e.type == InputType::EncoderRight || e.type == InputType::EncoderLeft)
        adjust(focus_, e.steps());
    else if (e.type == InputType::EncoderClick || e.type == InputType::ButtonPress) {
        focus_ = static_cast<uint8_t>((focus_ + 1) % 4);
        if (!scope_ && !focus_)
            focus_ = 1;
        render();
    }
}
size_t UIAllocationPage::consoleState(char* out, size_t cap, size_t len) {
    using namespace WaveX::Debug;
    const auto p = model_.Effective();
    const auto& s = model_.State();
    len = AppendKvInt(out, cap, len, "allocready", alive_ && model_.Ready());
    len = AppendKvInt(out, cap, len, "allocscope", scope_);
    len = AppendKvInt(out, cap, len, "allocmode", static_cast<uint8_t>(p.mode));
    len = AppendKvInt(out, cap, len, "alloclimit", p.limit);
    len = AppendKvInt(out, cap, len, "allocsteal", static_cast<uint8_t>(p.steal));
    len = AppendKvInt(out, cap, len, "allocinherit", s.inherited);
    len = AppendKvInt(out, cap, len, "allocdirty", s.dirty);
    len = AppendKvInt(out, cap, len, "allocrevision", s.revision);
    return AppendKvInt(out, cap, len, "allocfocus", focus_);
}
bool UIAllocationPage::consoleCommand(const char* args, char* reply, size_t cap) {
    char name[16], extra;
    int value;
    if (!args || std::sscanf(args, "%15s %d %c", name, &value, &extra) != 2 || !model_.Ready())
        return false;
    auto p = model_.Effective();
    bool inherit = scope_ && model_.State().inherited;
    if (!std::strcmp(name, "MODE") && value >= 0 && value <= 1) {
        p.mode = static_cast<A::PlayMode>(value);
        inherit = false;
    } else if (!std::strcmp(name, "LIMIT") && value >= 0 && value <= 8) {
        p.limit = static_cast<uint8_t>(value);
        inherit = false;
    } else if (!std::strcmp(name, "STEAL") && value >= 0 && value <= 2) {
        p.steal = static_cast<A::StealFrom>(value);
        inherit = false;
    } else if (!std::strcmp(name, "INHERIT") && scope_ && value >= 0 && value <= 1)
        inherit = value;
    else
        return false;
    if (!send(ALLOC_SET, p, inherit))
        return false;
    std::snprintf(reply, cap, "ok");
    return true;
}
void UIAllocationPage::tick(lv_timer_t* timer) {
    static_cast<UIAllocationPage*>(lv_timer_get_user_data(timer))->service();
}
std::shared_ptr<UIPage> createAllocationPage(uint8_t scope) {
    return std::make_shared<UIAllocationPage>(scope);
}
}  // namespace wavex_ui
