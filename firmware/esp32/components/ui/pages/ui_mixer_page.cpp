#include "ui/ui_mixer_page.h"

#include <esp_random.h>

#include "../styles/ui_theme.h"
#include "debug/console_command.h"
#include "inter_mcu.h"
#include "ui/current_track.h"
#include "ui/mixer_solo.h"
#include "ui/ui_navigator.h"

#include "audio/track_mix.hpp"
#include <algorithm>
#include <cmath>
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
void text(lv_obj_t* obj, const char* value) {
    if (std::strcmp(lv_label_get_text(obj), value))
        lv_label_set_text(obj, value);
}
void state(lv_obj_t* obj, lv_state_t flag, bool on) {
    if (lv_obj_has_state(obj, flag) == on)
        return;
    if (on)
        lv_obj_add_state(obj, flag);
    else
        lv_obj_remove_state(obj, flag);
}
}  // namespace
uint8_t UIMixerPage::track(uint8_t index) const {
    return index == 8 ? MIX_MASTER_TRACK : static_cast<uint8_t>(first_ + index);
}
void UIMixerPage::onEnter(lv_obj_t* parent) {
    root_ = lv_obj_create(parent);
    ui_theme_apply_container_style(root_, false);
    lv_obj_set_size(root_, UI_CONTENT_WIDTH, UI_CONTENT_HEIGHT);
    lv_obj_set_style_pad_all(root_, 0, 0);
    lv_obj_remove_flag(root_, LV_OBJ_FLAG_SCROLLABLE);
    status_ = lv_label_create(root_);
    ui_theme_apply_label_style(status_, false);
    lv_obj_set_style_text_font(status_, UI_FONT_SMALL, 0);
    lv_obj_set_pos(status_, UI_MARGIN_X, UI_PADDING_MEDIUM);
    lv_obj_set_width(status_, UI_CONTENT_WIDTH - 2 * UI_MARGIN_X);
    constexpr int width = (UI_CONTENT_WIDTH - 2 * UI_MARGIN_X - 8 * UI_GUTTER) / 9;
    for (uint8_t i = 0; i < strips_.size(); ++i) {
        auto& strip = strips_[i];
        strip.owner = this;
        strip.index = i;
        strip.card = lv_obj_create(root_);
        ui_theme_apply_container_style(strip.card, true);
        lv_obj_set_style_pad_all(strip.card, 0, 0);
        lv_obj_remove_flag(strip.card, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_pos(strip.card, UI_MARGIN_X + i * (width + UI_GUTTER), UI_MIX_BUTTON_HEIGHT);
        lv_obj_set_size(
            strip.card, width, UI_CONTENT_HEIGHT - UI_MIX_BUTTON_HEIGHT - UI_PADDING_MEDIUM);
        auto button = [&](int y, const char* value, lv_obj_t** label) {
            auto* obj = lv_button_create(strip.card);
            ui_theme_apply_button_style(obj, false);
            lv_obj_set_style_bg_color(obj, UI_COLOR_CARD_ALT, 0);
            lv_obj_set_style_bg_color(obj, UI_COLOR_ACCENT, LV_STATE_CHECKED);
            lv_obj_set_pos(obj, UI_PADDING_SMALL, y - UI_MIX_BUTTON_HEIGHT);
            lv_obj_set_size(obj, width - 2 * UI_PADDING_SMALL, UI_MIX_BUTTON_HEIGHT);
            auto* title = lv_label_create(obj);
            ui_theme_apply_label_style(title, false);
            lv_obj_set_style_text_font(title, UI_FONT_SMALL, 0);
            lv_label_set_text(title, value);
            lv_obj_center(title);
            if (label)
                *label = title;
            lv_obj_add_event_cb(obj, event, LV_EVENT_CLICKED, &strip);
            return obj;
        };
        button(UI_MIX_BUTTON_HEIGHT, "", &strip.title);
        strip.slider = lv_slider_create(strip.card);
        lv_slider_set_range(strip.slider, 0, 6600);
        lv_obj_set_pos(strip.slider,
                       (width - UI_MIX_FADER_WIDTH) / 2,
                       UI_MIX_FADER_TOP - UI_MIX_BUTTON_HEIGHT);
        lv_obj_set_size(strip.slider, UI_MIX_FADER_WIDTH, UI_MIX_FADER_HEIGHT);
        lv_obj_set_style_bg_color(strip.slider, UI_COLOR_CARD_ALT, LV_PART_MAIN);
        lv_obj_set_style_bg_color(strip.slider, UI_COLOR_ACCENT, LV_PART_INDICATOR);
        lv_obj_set_style_bg_color(strip.slider, UI_COLOR_FG, LV_PART_KNOB);
        lv_obj_set_style_pad_all(strip.slider, 0, LV_PART_KNOB);
        lv_obj_add_event_cb(strip.slider, event, LV_EVENT_PRESSED, &strip);
        lv_obj_add_event_cb(strip.slider, event, LV_EVENT_RELEASED, &strip);
        strip.level = lv_label_create(strip.card);
        ui_theme_apply_label_style(strip.level, false);
        lv_obj_set_style_text_font(strip.level, UI_FONT_SMALL, 0);
        lv_obj_set_size(strip.level, width, LV_SIZE_CONTENT);
        lv_obj_set_style_text_align(strip.level, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_pos(strip.level, 0, UI_MIX_LEVEL_TOP - UI_MIX_BUTTON_HEIGHT);
        if (i != 8) {
            strip.meter = lv_bar_create(strip.card);
            lv_obj_set_pos(strip.meter,
                           width - UI_PADDING_LARGE - UI_GUTTER,
                           UI_MIX_FADER_TOP - UI_MIX_BUTTON_HEIGHT);
            lv_obj_set_size(strip.meter, UI_GUTTER, UI_MIX_FADER_HEIGHT);
            lv_bar_set_range(strip.meter, 0, 255);
            lv_obj_set_style_bg_color(strip.meter, UI_COLOR_CARD_ALT, LV_PART_MAIN);
            lv_obj_set_style_bg_color(strip.meter, UI_COLOR_OK, LV_PART_INDICATOR);
            strip.pan = button(UI_MIX_PAN_TOP, "Center", &strip.pan_label);
            strip.mute = button(UI_MIX_MUTE_TOP, "Mute", nullptr);
            strip.solo = button(UI_MIX_SOLO_TOP, "Solo", nullptr);
        }
    }
    first_ = getCurrentTrack() < 8 ? 0 : 8;
    selected_ = getCurrentTrack() - first_;
    pan_focus_ = false;
    alive_ = inter_mcu_backend_link_alive();
    solo_sync_ = true;
    meter_sub_at_ = lv_tick_get() - 1000;
    reset();
    timer_ = lv_timer_create(tick, 50, this);
    service();
}
void UIMixerPage::onExit() {
    inter_mcu_send_mix_op(MIX_OP_UNSUB_METERS, 0, 0);
    if (timer_)
        lv_timer_delete(timer_);
    timer_ = nullptr;
    if (root_)
        lv_obj_delete(root_);
    root_ = status_ = nullptr;
    strips_ = {};
    pending_ = false;
}
void UIMixerPage::reset() {
    for (uint8_t i = 0; i < strips_.size(); ++i)
        strips_[i].model.Reset(track(i));
    pending_ = false;
    poll_ = 0;
    sent_at_ = lv_tick_get() - 100;
    soft_state_ = UINT32_MAX;
}
void UIMixerPage::read() {
    if (!alive_)
        return;
    MixStateRequest request{nextId(), track(poll_)};
    sent_at_ = lv_tick_get();
    if (inter_mcu_request_mix_state(request) == ESP_OK) {
        strips_[poll_].model.Expect(request.request_id);
        pending_ = true;
    }
}
void UIMixerPage::service() {
    const bool alive = inter_mcu_backend_link_alive();
    if (alive_ != alive) {
        alive_ = alive;
        solo_sync_ = true;
        meter_sub_at_ = lv_tick_get() - 1000;
        reset();
    }
    if (alive_ && solo_sync_ &&
        inter_mcu_send_mix_op(MIX_OP_SET_SOLO_MASK, 0, mixerSolo.Mask()) == ESP_OK)
        solo_sync_ = false;
    if (alive_ && lv_tick_elaps(meter_sub_at_) >= 1000 &&
        inter_mcu_send_mix_op(MIX_OP_SUB_METERS, 0, 0) == ESP_OK)
        meter_sub_at_ = lv_tick_get();
    MixStateMessage reply;
    if (pending_ && inter_mcu_get_mix_state(&reply) && strips_[poll_].model.Accept(reply)) {
        strips_[poll_].accepted_at = lv_tick_get();
        pending_ = false;
        poll_ = static_cast<uint8_t>((poll_ + 1) % strips_.size());
    }
    for (uint8_t i = 0; i < strips_.size(); ++i) {
        auto& strip = strips_[i];
        if (strip.model.Valid() && lv_tick_elaps(strip.accepted_at) > 2000) {
            strip.model.Reset(track(i));
            if (pending_ && poll_ == i)
                pending_ = false;
        }
    }
    if (pending_ && lv_tick_elaps(sent_at_) > 500) {
        strips_[poll_].model.Block();
        pending_ = false;
        poll_ = static_cast<uint8_t>((poll_ + 1) % strips_.size());
    }
    if (!pending_ && lv_tick_elaps(sent_at_) >= 50)
        read();
    render();
}
void UIMixerPage::select(uint8_t index) {
    if (index >= strips_.size())
        return;
    selected_ = index;
    if (index != 8)
        setCurrentTrack(track(index));
    else
        pan_focus_ = false;
    render();
}
void UIMixerPage::onTrackChanged() {
    if (!root_)
        return;
    const uint8_t first = getCurrentTrack() < 8 ? 0 : 8;
    if (first != first_) {
        first_ = first;
        reset();
    }
    select(getCurrentTrack() - first_);
}
bool UIMixerPage::set(uint8_t index, uint8_t op, uint16_t value) {
    if (!alive_ || index >= strips_.size() || !strips_[index].model.Ready())
        return false;
    if (inter_mcu_send_mix_op(op, track(index), value) != ESP_OK)
        return false;
    strips_[index].model.Block();
    // Prioritize readback of this edit; old replies cannot unlock its controls.
    pending_ = false;
    poll_ = index;
    read();
    render();
    return true;
}
bool UIMixerPage::solo(uint8_t index) {
    if (!alive_ || index >= 8)
        return false;
    const auto next = mixerSolo.Contains(track(index)) ? uint8_t{0xff} : track(index);
    if (inter_mcu_send_mix_op(MIX_OP_SET_SOLO_MASK, 0, MixerSolo::Mask(next)) != ESP_OK)
        return false;
    mixerSolo.Select(next);
    render();
    return true;
}
void UIMixerPage::adjust(int delta) {
    const auto& model = strips_[selected_].model;
    if (!model.Ready())
        return;
    const auto& value = model.State();
    if (pan_focus_ && selected_ != 8) {
        const int pan = static_cast<int>(std::lround(WaveX::Mix::WireToPan(value.pan) * 100));
        const int next = std::clamp(pan + delta, -100, 100);
        set(selected_,
            MIX_OP_SET_PAN,
            next == 0 ? 32768 : WaveX::Mix::PanToWire(static_cast<float>(next) / 100));
    } else {
        set(selected_,
            selected_ == 8 ? MIX_OP_SET_MASTER : MIX_OP_SET_GAIN,
            static_cast<uint16_t>(std::clamp(static_cast<int>(value.gain) + delta * 50, 0, 6600)));
    }
}
void UIMixerPage::page() {
    first_ = first_ ? 0 : 8;
    reset();
    select(selected_);
    service();
}
void UIMixerPage::event(lv_event_t* e) {
    auto& strip = *static_cast<Strip*>(lv_event_get_user_data(e));
    auto& page = *strip.owner;
    auto* obj = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
    auto code = lv_event_get_code(e);
    // Do not redraw the slider before reading the user's release value.
    if (obj == strip.slider && code == LV_EVENT_RELEASED) {
        page.set(strip.index,
                 strip.index == 8 ? MIX_OP_SET_MASTER : MIX_OP_SET_GAIN,
                 static_cast<uint16_t>(lv_slider_get_value(obj)));
        page.render();
        return;
    }
    if (obj == strip.pan)
        page.pan_focus_ = true;
    else if (obj == strip.slider)
        page.pan_focus_ = false;
    page.select(strip.index);
    if (obj == strip.mute && strip.model.Ready())
        page.set(strip.index, MIX_OP_SET_MUTE, !strip.model.State().mute);
    else if (obj == strip.solo)
        page.solo(strip.index);
}
void UIMixerPage::render() {
    if (!root_)
        return;
    char value[48];
    MixMetersMessage meters;
    const bool have_meters = alive_ && inter_mcu_get_mix_meters(&meters);
    for (uint8_t i = 0; i < strips_.size(); ++i) {
        auto& strip = strips_[i];
        const auto& model = strip.model;
        const auto& mix = model.State();
        if (i == 8)
            std::snprintf(value, sizeof(value), "Master");
        else
            std::snprintf(value, sizeof(value), "Track %u", track(i) + 1);
        text(strip.title, value);
        const bool valid = alive_ && model.Valid(), ready = alive_ && model.Ready();
        if (!valid)
            std::snprintf(value, sizeof(value), "--");
        else if (!mix.gain)
            std::snprintf(value, sizeof(value), "-inf dB");
        else
            std::snprintf(value,
                          sizeof(value),
                          "%.1f dB",
                          static_cast<double>(WaveX::Mix::WireToGainDb(mix.gain)));
        text(strip.level, value);
        if (!lv_slider_is_dragged(strip.slider) && lv_slider_get_value(strip.slider) != mix.gain)
            lv_slider_set_value(strip.slider, mix.gain, LV_ANIM_OFF);
        state(strip.slider, LV_STATE_DISABLED, !ready);
        const auto border = i == selected_ ? UI_COLOR_ACCENT : UI_COLOR_LINE;
        if (!lv_color_eq(lv_obj_get_style_border_color(strip.card, LV_PART_MAIN), border))
            lv_obj_set_style_border_color(strip.card, border, 0);
        if (i == 8)
            continue;
        const int peak = have_meters ? meters.peak[track(i)] : 0;
        if (lv_bar_get_value(strip.meter) != peak)
            lv_bar_set_value(strip.meter, peak, LV_ANIM_OFF);
        if (!valid)
            std::snprintf(value, sizeof(value), "--");
        else {
            const int pan = static_cast<int>(std::lround(WaveX::Mix::WireToPan(mix.pan) * 100));
            if (!pan)
                std::snprintf(value, sizeof(value), "Center");
            else
                std::snprintf(value, sizeof(value), "%s %d", pan < 0 ? "L" : "R", std::abs(pan));
        }
        text(strip.pan_label, value);
        state(strip.pan, LV_STATE_CHECKED, i == selected_ && pan_focus_);
        state(strip.pan, LV_STATE_DISABLED, !ready);
        state(strip.mute, LV_STATE_CHECKED, valid && mix.mute);
        state(strip.mute, LV_STATE_DISABLED, !ready);
        state(strip.solo, LV_STATE_CHECKED, mixerSolo.Contains(track(i)));
        state(strip.solo, LV_STATE_DISABLED, !alive_);
    }
    text(status_,
         !alive_
             ? "Audio engine disconnected"
             : (pan_focus_ ? "Pan / balance: turn encoder or use - / +"
                           : "Level: drag and release fader, or turn encoder for 0.5 dB steps"));
    uint32_t soft = selected_ | (static_cast<uint32_t>(first_) << 4) | (pan_focus_ ? 256u : 0u) |
                    (alive_ ? 512u : 0u) | (strips_[selected_].model.Ready() ? 1024u : 0u);
    if (soft_state_ != soft) {
        soft_state_ = soft;
        UINavigator::instance().refreshSoftkeys();
    }
}
std::array<Softkey, NUM_SOFTKEYS> UIMixerPage::getSoftkeys() {
    std::array<Softkey, NUM_SOFTKEYS> keys{};
    const bool ready = alive_ && strips_[selected_].model.Ready();
    keys[0] = {"Back", [] { UINavigator::instance().pop(); }};
    keys[1] = {first_ ? "Tracks 1-8" : "Tracks 9-16", [this] { page(); }};
    keys[2] = {"Project", [] { UINavigator::instance().jumpToRoot(RootGroup::Track); }};
    keys[3] = {pan_focus_ ? "Pan -" : "Level -", [this] { adjust(-1); }, ready, "Reading settings"};
    keys[4] = {pan_focus_ ? "Pan +" : "Level +", [this] { adjust(1); }, ready, "Reading settings"};
    keys[5] = {"Master", [this] { select(8); }};
    return keys;
}
void UIMixerPage::onInput(const InputEvent& event) {
    if (event.type == InputType::EncoderRight || event.type == InputType::EncoderLeft)
        adjust(event.steps());
    else if (event.type == InputType::EncoderClick || event.type == InputType::ButtonPress) {
        pan_focus_ = selected_ != 8 && !pan_focus_;
        render();
    }
}
size_t UIMixerPage::consoleState(char* out, size_t cap, size_t len) {
    using namespace WaveX::Debug;
    const auto& model = strips_[selected_].model;
    len = AppendKvInt(out, cap, len, "mixtrack", selected_ == 8 ? 0 : track(selected_) + 1);
    len = AppendKvInt(out, cap, len, "mixready", alive_ && model.Ready());
    len = AppendKvInt(out, cap, len, "mixgain", model.State().gain);
    len = AppendKvInt(out, cap, len, "mixpan", model.State().pan);
    len = AppendKvInt(out, cap, len, "mixmute", model.State().mute);
    return AppendKvInt(out, cap, len, "mixsolo", mixerSolo.Active() ? mixerSolo.Track() + 1 : 0);
}
bool UIMixerPage::consoleCommand(const char* args, char* reply, size_t cap) {
    char field[12], extra;
    int value;
    if (!args || std::sscanf(args, "%11s %d %c", field, &value, &extra) != 2)
        return false;
    bool ok = false;
    if (!std::strcmp(field, "SELECT") && value >= 0 && value <= 16) {
        if (value == 0)
            select(8);
        else {
            setCurrentTrack(static_cast<uint8_t>(value - 1));
            onTrackChanged();
        }
        ok = true;
    } else if (!std::strcmp(field, "LEVEL") && value >= 0 && value <= 6600)
        ok = set(selected_,
                 selected_ == 8 ? MIX_OP_SET_MASTER : MIX_OP_SET_GAIN,
                 static_cast<uint16_t>(value));
    else if (!std::strcmp(field, "PAN") && selected_ != 8 && value >= 0 && value <= 65535)
        ok = set(selected_, MIX_OP_SET_PAN, static_cast<uint16_t>(value));
    else if (!std::strcmp(field, "MUTE") && selected_ != 8 && value >= 0 && value <= 1)
        ok = set(selected_, MIX_OP_SET_MUTE, static_cast<uint16_t>(value));
    else if (!std::strcmp(field, "SOLO") && selected_ != 8 && value == 1)
        ok = solo(selected_);
    if (ok)
        std::snprintf(reply, cap, "ok");
    return ok;
}
void UIMixerPage::tick(lv_timer_t* timer) {
    static_cast<UIMixerPage*>(lv_timer_get_user_data(timer))->service();
}
std::shared_ptr<UIPage> createMixerPage() {
    return std::make_shared<UIMixerPage>();
}
}  // namespace wavex_ui
