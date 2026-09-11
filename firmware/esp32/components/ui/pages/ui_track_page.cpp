#include "ui/ui_track_page.h"

#include <esp_random.h>

#include "../styles/ui_theme.h"
#include "debug/console_command.h"
#include "inter_mcu.h"
#include "ui/current_track.h"
#include "ui/ui_navigator.h"

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
void label(lv_obj_t* o, const char* value) {
    if (o && std::strcmp(lv_label_get_text(o), value))
        lv_label_set_text(o, value);
}
}  // namespace
void UITrackPage::onEnter(lv_obj_t* parent) {
    root_ = lv_obj_create(parent);
    ui_theme_apply_container_style(root_, false);
    lv_obj_set_size(root_, UI_CONTENT_WIDTH, UI_CONTENT_HEIGHT);
    lv_obj_set_style_pad_all(root_, 0, 0);
    lv_obj_remove_flag(root_, LV_OBJ_FLAG_SCROLLABLE);
    const int width = (UI_CONTENT_WIDTH - 2 * UI_MARGIN_X - 3 * UI_GUTTER) / 4;
    for (uint8_t i = 0; i < 8; ++i) {
        auto& c = cells_[i];
        c.owner = this;
        c.index = i;
        c.object = lv_button_create(root_);
        ui_theme_apply_button_style(c.object, false);
        lv_obj_set_style_bg_color(c.object, UI_COLOR_CARD, 0);
        lv_obj_set_style_bg_color(c.object, UI_COLOR_CARD_ALT, LV_STATE_CHECKED);
        lv_obj_set_style_bg_color(c.object, UI_COLOR_CARD_ALT, LV_STATE_PRESSED);
        lv_obj_set_style_border_color(c.object, UI_COLOR_ACCENT, LV_STATE_CHECKED);
        lv_obj_set_style_border_width(c.object, UI_BORDER_WIDTH_FOCUS, LV_STATE_CHECKED);
        lv_obj_set_pos(c.object,
                       UI_MARGIN_X + (i % 4) * (width + UI_GUTTER),
                       UI_PADDING_MEDIUM + (i / 4) * 100);
        lv_obj_set_size(c.object, width, 88);
        c.label = lv_label_create(c.object);
        ui_theme_apply_label_style(c.label, false);
        lv_obj_center(c.label);
        lv_obj_add_event_cb(c.object, choose, LV_EVENT_CLICKED, &c);
    }
    heading_ = lv_label_create(root_);
    ui_theme_apply_label_style(heading_, true);
    lv_obj_set_pos(heading_, UI_MARGIN_X, 232);
    lv_obj_set_width(heading_, UI_CONTENT_WIDTH - 2 * UI_MARGIN_X);
    lv_label_set_long_mode(heading_, LV_LABEL_LONG_WRAP);
    midi_ = valueTileCreate(root_, UI_MARGIN_X, 320, 350, 190, "MIDI INPUT", "");
    valueTileHideFill(midi_);
    valueTileSetOnAdjust(midi_, [this](int delta) { adjust(delta); });
    status_ = lv_label_create(root_);
    ui_theme_apply_label_style(status_, false);
    lv_obj_set_style_text_font(status_, UI_FONT_SMALL, 0);
    lv_obj_set_pos(status_, 420, 340);
    lv_obj_set_width(status_, UI_CONTENT_WIDTH - 440);
    lv_label_set_long_mode(status_, LV_LABEL_LONG_WRAP);
    timer_ = lv_timer_create(tick, 100, this);
    onTrackChanged();
}
void UITrackPage::onExit() {
    if (timer_)
        lv_timer_delete(timer_);
    timer_ = nullptr;
    if (root_)
        lv_obj_delete(root_);
    root_ = heading_ = status_ = nullptr;
    midi_ = {};
    pending_ = false;
    model_.Reset(getCurrentTrack());
}
void UITrackPage::onTrackChanged() {
    first_ = getCurrentTrack() < 8 ? 0 : 8;
    model_.Reset(getCurrentTrack());
    alive_ = inter_mcu_backend_link_alive();
    pending_ = false;
    soft_state_ = UINT32_MAX;
    read();
    render();
}
void UITrackPage::read() {
    if (!alive_)
        return;
    TrackStateRequest r{nextId(), getCurrentTrack()};
    read_at_ = lv_tick_get();
    if (inter_mcu_request_track_state(r) == ESP_OK) {
        read_id_ = r.request_id;
        model_.Expect(read_id_);
        pending_ = true;
    }
}
void UITrackPage::service() {
    const bool alive = inter_mcu_backend_link_alive();
    if (alive != alive_) {
        onTrackChanged();
        return;
    }
    TrackStateMessage s{};
    if (pending_ && inter_mcu_get_track_state(&s) && model_.Accept(s)) {
        pending_ = false;
        accepted_at_ = lv_tick_get();
    }
    if (model_.Valid() && lv_tick_elaps(accepted_at_) > 2000)
        model_.Reset(getCurrentTrack());
    if ((pending_ && lv_tick_elaps(read_at_) > 1500) ||
        (!pending_ && lv_tick_elaps(read_at_) > 500))
        read();
    render();
}
void UITrackPage::select(uint8_t track) {
    if (track >= 16)
        return;
    setCurrentTrack(track);
    inter_mcu_request_track_binding(track);
    onTrackChanged();
}
void UITrackPage::choose(lv_event_t* e) {
    auto* c = static_cast<Cell*>(lv_event_get_user_data(e));
    c->owner->select(c->owner->first_ + c->index);
}
void UITrackPage::setMidi(uint8_t value) {
    if (!alive_ || !model_.Ready() || !TrackMidiInValid(value))
        return;
    if (inter_mcu_send_track_op(TRACK_OP_SET_MIDI_IN, getCurrentTrack(), value) != ESP_OK) {
        label(status_, "Link busy. Try again.");
        return;
    }
    model_.Reset(getCurrentTrack());
    pending_ = false;
    read();
    render();
}
void UITrackPage::adjust(int delta) {
    if (model_.Ready())
        setMidi(TrackPageModel::StepMidi(model_.State().midi_in, delta));
}
void UITrackPage::render() {
    if (!root_)
        return;
    char text[160];
    for (uint8_t i = 0; i < 8; ++i) {
        std::snprintf(text, sizeof(text), "Track %u", trackDisplayNumber(first_ + i));
        label(cells_[i].label, text);
        const bool selected = first_ + i == getCurrentTrack();
        if (lv_obj_has_state(cells_[i].object, LV_STATE_CHECKED) != selected) {
            if (selected)
                lv_obj_add_state(cells_[i].object, LV_STATE_CHECKED);
            else
                lv_obj_remove_state(cells_[i].object, LV_STATE_CHECKED);
        }
    }
    const auto& s = model_.State();
    std::snprintf(text,
                  sizeof(text),
                  "Track %u / %s",
                  trackDisplayNumber(getCurrentTrack()),
                  !model_.Valid()
                      ? "Reading..."
                      : (!s.loaded ? "Empty" : (s.name[0] ? s.name : "Sample Instrument")));
    label(heading_, text);
    if (!model_.Valid())
        std::snprintf(text, sizeof(text), "--");
    else if (s.midi_in == TRACK_MIDI_IN_OFF)
        std::snprintf(text, sizeof(text), "Off");
    else if (s.midi_in == TRACK_MIDI_IN_OMNI)
        std::snprintf(text, sizeof(text), "Omni");
    else
        std::snprintf(text, sizeof(text), "%u", s.midi_in);
    if (std::strcmp(lv_label_get_text(midi_.value), text))
        valueTileSetValue(midi_, text);
    valueTileSetFocus(midi_, alive_ && model_.Ready());
    label(status_,
          !alive_
              ? "Audio engine disconnected"
              : (!model_.Valid()
                     ? "Reading Track settings..."
                     : (s.busy ? "Instrument loading..."
                               : "MIDI input chooses which external notes reach this Track.\nPads "
                                 "and sequencer steps address the Track directly.")));
    uint32_t state = (alive_ && model_.Ready() ? 1u : 0u) | (static_cast<uint32_t>(first_) << 1);
    if (state != soft_state_) {
        soft_state_ = state;
        UINavigator::instance().refreshSoftkeys();
    }
}
std::array<Softkey, NUM_SOFTKEYS> UITrackPage::getSoftkeys() {
    std::array<Softkey, NUM_SOFTKEYS> k{};
    k[0] = {"Back", [] { UINavigator::instance().pop(); }};
    k[1] = {"Instrument", [] { UINavigator::instance().jumpToRoot(RootGroup::Instrument); }};
    k[2] = {"Browse", [] { UINavigator::instance().jumpToRoot(RootGroup::Sample); }};
    k[3] = {"MIDI -", [this] { adjust(-1); }, alive_ && model_.Ready(), "Reading Track"};
    k[4] = {"MIDI +", [this] { adjust(1); }, alive_ && model_.Ready(), "Reading Track"};
    k[5] = {first_ ? "Tracks 1-8" : "Tracks 9-16", [this] {
                first_ = first_ ? 0 : 8;
                render();
            }};
    return k;
}
void UITrackPage::onInput(const InputEvent& e) {
    if (e.type == InputType::EncoderRight || e.type == InputType::EncoderLeft)
        adjust(e.steps());
}
size_t UITrackPage::consoleState(char* out, size_t cap, size_t len) {
    using namespace WaveX::Debug;
    len = AppendKvInt(out, cap, len, "trackready", alive_ && model_.Ready());
    len = AppendKvInt(out, cap, len, "trackfirst", first_ + 1);
    len = AppendKvInt(out, cap, len, "trackloaded", model_.Valid() && model_.State().loaded);
    return AppendKvInt(out, cap, len, "midiin", model_.State().midi_in);
}
bool UITrackPage::consoleCommand(const char* args, char* reply, size_t cap) {
    char field[12], extra;
    int value;
    if (!args || std::sscanf(args, "%11s %d %c", field, &value, &extra) != 2)
        return false;
    if (!std::strcmp(field, "SELECT") && value >= 1 && value <= 16)
        select(static_cast<uint8_t>(value - 1));
    else if (!std::strcmp(field, "MIDI") && value >= 0 && value <= 255 && TrackMidiInValid(value) &&
             alive_ && model_.Ready())
        setMidi(static_cast<uint8_t>(value));
    else
        return false;
    std::snprintf(reply, cap, "ok");
    return true;
}
void UITrackPage::tick(lv_timer_t* t) {
    static_cast<UITrackPage*>(lv_timer_get_user_data(t))->service();
}
std::shared_ptr<UIPage> createTrackPage() {
    return std::make_shared<UITrackPage>();
}
}  // namespace wavex_ui
