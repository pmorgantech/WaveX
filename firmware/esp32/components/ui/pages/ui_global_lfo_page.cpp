#include "ui/ui_global_lfo_page.h"

#include <esp_random.h>

#include "debug/console_command.h"
#include "inter_mcu.h"
#include "ui/ui_navigator.h"
#include "ui_theme.h"

#include <algorithm>
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
constexpr const char* titles[] = {"WAVE", "RATE Hz", "SYNC", "RESTART"};
constexpr const char* waves[] = {"Sine", "Triangle", "Saw", "Square", "Sample & hold"};
constexpr const char* restarts[] = {"Free", "Transport", "Any note"};
}  // namespace
void UIGlobalLfoPage::onEnter(lv_obj_t* parent) {
    root_ = lv_obj_create(parent);
    ui_theme_apply_container_style(root_, false);
    lv_obj_set_size(root_, UI_CONTENT_WIDTH, UI_CONTENT_HEIGHT);
    lv_obj_set_style_pad_all(root_, 0, 0);
    lv_obj_remove_flag(root_, LV_OBJ_FLAG_SCROLLABLE);
    const int width = (UI_CONTENT_WIDTH - 2 * UI_MARGIN_X - 3 * UI_GUTTER) / 4;
    for (uint8_t i = 0; i < 4; ++i) {
        tiles_[i] = valueTileCreate(root_,
                                    UI_MARGIN_X + i * (width + UI_GUTTER),
                                    UI_PADDING_LARGE,
                                    width,
                                    UI_PERFORMANCE_TILE_HEIGHT,
                                    titles[i],
                                    "");
        valueTileSetOnAdjust(tiles_[i], [this, i](int delta) { adjust(i, delta); });
    }
    status_ = lv_label_create(root_);
    ui_theme_apply_label_style(status_, false);
    lv_obj_set_pos(status_, UI_MARGIN_X, UI_GLOBAL_LFO_STATUS_Y);
    lv_obj_set_width(status_, UI_CONTENT_WIDTH - 2 * UI_MARGIN_X);
    lv_obj_set_style_text_font(status_, UI_FONT_BODY, 0);
    lv_label_set_long_mode(status_, LV_LABEL_LONG_WRAP);
    valid_ = false;
    pending_ = expected_ = 0;
    read_at_ = 0;
    drawn_ready_ = false;
    timer_ = lv_timer_create(
        [](lv_timer_t* timer) {
            static_cast<UIGlobalLfoPage*>(lv_timer_get_user_data(timer))->service();
        },
        67,
        this);
    service();
}
void UIGlobalLfoPage::onExit() {
    if (timer_)
        lv_timer_delete(timer_);
    timer_ = nullptr;
    if (root_)
        lv_obj_delete(root_);
    root_ = status_ = nullptr;
}
bool UIGlobalLfoPage::ready() const {
    return alive_ && valid_ && !pending_ && lv_tick_get() - received_at_ < 1500;
}
void UIGlobalLfoPage::read() {
    GlobalLfoOpMessage m;
    m.request_id = nextId();
    if (inter_mcu_send_global_lfo(m) == ESP_OK)
        expected_ = m.request_id;
    read_at_ = lv_tick_get();
}
void UIGlobalLfoPage::send(uint8_t op) {
    if (!ready())
        return;
    GlobalLfoOpMessage m;
    m.request_id = nextId();
    m.revision = state_.revision;
    m.op = op;
    m.value = desired_;
    if (inter_mcu_send_global_lfo(m) == ESP_OK) {
        pending_ = expected_ = m.request_id;
        sent_at_ = read_at_ = lv_tick_get();
    }
    drawn_ready_ = ready();
    render();
    UINavigator::instance().refreshSoftkeys();
}
void UIGlobalLfoPage::service() {
    const bool alive = inter_mcu_backend_link_alive();
    if (alive != alive_) {
        alive_ = alive;
        valid_ = false;
        pending_ = 0;
        if (alive_)
            read();
    }
    GlobalLfoSyncMessage m;
    if (alive_ && inter_mcu_get_global_lfo(&m) && m.request_id == expected_ && m.revision &&
        m.error <= 1 && IsValidGlobalLfoSettings(m.value)) {
        state_ = m;
        valid_ = true;
        received_at_ = lv_tick_get();
        if (!pending_ || m.completed_request_id == pending_) {
            pending_ = 0;
            desired_ = m.value;
        }
    }
    if (pending_ && lv_tick_get() - sent_at_ >= 5000) {
        pending_ = 0;
        valid_ = false;
        read();
    }
    if (alive_ && lv_tick_get() - read_at_ >= 300)
        read();
    render();
    if (drawn_ready_ != ready()) {
        drawn_ready_ = ready();
        UINavigator::instance().refreshSoftkeys();
    }
}
void UIGlobalLfoPage::render() {
    char text[64];
    valueTileSetValue(tiles_[0], valid_ ? waves[desired_.wave] : "...", true);
    std::snprintf(text, sizeof(text), "%.2f", double(desired_.rate_hz));
    valueTileSetValue(tiles_[1], valid_ ? text : "...", true);
    valueTileSetValue(
        tiles_[2], valid_ ? WaveX::LfoControl::DivisionLabel(desired_.sync_div) : "...", true);
    valueTileSetValue(tiles_[3], valid_ ? restarts[desired_.restart] : "...", true);
    char message[384];
    std::snprintf(message,
                  sizeof(message),
                  "%s\nOne global LFO shared by all Tracks. Route Global LFO in the modulation "
                  "matrix.\nSettings belong to this session. Instrument Apply/Revert and Save do "
                  "not change them.\nSync uses tempo; Rate Hz applies when Sync is Off.",
                  !alive_        ? "Disconnected"
                  : !valid_      ? "Reading global LFO..."
                  : pending_     ? "Updating..."
                  : state_.error ? "Settings changed; current values restored"
                                 : "Global LFO");
    if (std::strcmp(lv_label_get_text(status_), message))
        lv_label_set_text(status_, message);
}
void UIGlobalLfoPage::adjust(uint8_t field, int delta) {
    if (!ready() || !delta)
        return;
    switch (field) {
        case 0:
            desired_.wave = std::clamp(int(desired_.wave) + delta, 0, 4);
            break;
        case 1:
            desired_.rate_hz = std::clamp(desired_.rate_hz + delta * .01f, .01f, 100.f);
            break;
        case 2:
            desired_.sync_div = std::clamp(
                int(desired_.sync_div) + delta, 0, int(WaveX::LfoControl::kDivisionCount) - 1);
            break;
        case 3:
            desired_.restart = std::clamp(int(desired_.restart) + delta, 0, 2);
            break;
        default:
            return;
    }
    send(GLOBAL_LFO_SET);
}
EncoderBindings UIGlobalLfoPage::encoderBindings() {
    EncoderBindings result{};
    for (uint8_t i = 0; i < 4; ++i) {
        auto& b = result[i];
        b.label = titles[i];
        b.owner = const_cast<UIGlobalLfoPage*>(this);
        b.parameter = i;
        b.enabled = ready();
        b.coarse_step = i == 1 ? 10 : 1;
        b.onSteps = [](void* self, uint8_t field, int delta) {
            static_cast<UIGlobalLfoPage*>(self)->adjust(field, delta);
        };
    }
    return result;
}
std::array<Softkey, NUM_SOFTKEYS> UIGlobalLfoPage::getSoftkeys() {
    std::array<Softkey, NUM_SOFTKEYS> keys{};
    keys[0] = {"Back", [] { UINavigator::instance().pop(); }};
    keys[5] = {
        "Reset phase", [this] { send(GLOBAL_LFO_RESET); }, ready(), "Waiting for current settings"};
    return keys;
}
size_t UIGlobalLfoPage::consoleState(char* out, size_t cap, size_t len) {
    len = WaveX::Debug::AppendKvInt(out, cap, len, "globalready", ready());
    len = WaveX::Debug::AppendKvInt(out, cap, len, "globalwave", state_.value.wave);
    len = WaveX::Debug::AppendKvInt(
        out, cap, len, "globalrate", int(state_.value.rate_hz * 100 + .5f));
    len = WaveX::Debug::AppendKvInt(out, cap, len, "globalsync", state_.value.sync_div);
    return WaveX::Debug::AppendKvInt(out, cap, len, "globalrestart", state_.value.restart);
}
bool UIGlobalLfoPage::consoleCommand(const char* args, char* reply, size_t cap) {
    unsigned field;
    int delta;
    char extra;
    if (!args || std::sscanf(args, "ADJUST %u %d %c", &field, &delta, &extra) != 2 || field > 3 ||
        !ready())
        return false;
    adjust(field, delta);
    std::snprintf(reply, cap, "ok");
    return true;
}
std::shared_ptr<UIPage> createGlobalLfoPage() {
    return std::make_shared<UIGlobalLfoPage>();
}
}  // namespace wavex_ui
