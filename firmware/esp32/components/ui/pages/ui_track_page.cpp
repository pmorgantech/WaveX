#include "ui/ui_track_page.h"

#include <esp_random.h>

#include "../styles/ui_theme.h"
#include "debug/console_command.h"
#include "inter_mcu.h"
#include "ui/current_track.h"
#include "ui/ui_allocation_page.h"
#include "ui/ui_bank_page.h"
#include "ui/ui_navigator.h"
#include "ui/ui_project_files_page.h"
#include "ui/ui_sample_browser.h"

#include "audio/track_mix.hpp"
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
    lv_obj_set_width(heading_,
                     UI_CONTENT_WIDTH - 2 * UI_MARGIN_X - UI_PROJECT_FILES_BUTTON_W - UI_GUTTER);
    auto* files = lv_button_create(root_);
    ui_theme_apply_button_style(files, false);
    lv_obj_set_pos(files,
                   UI_CONTENT_WIDTH - UI_MARGIN_X - UI_PROJECT_FILES_BUTTON_W,
                   UI_PROJECT_FILES_BUTTON_Y);
    lv_obj_set_size(files, UI_PROJECT_FILES_BUTTON_W, UI_PROJECT_FILES_BUTTON_H);
    auto* files_label = lv_label_create(files);
    ui_theme_apply_label_style(files_label, false);
    lv_label_set_text(files_label, "Project files");
    lv_obj_center(files_label);
    lv_obj_add_event_cb(
        files,
        [](lv_event_t*) { UINavigator::instance().push(createProjectFilesPage()); },
        LV_EVENT_CLICKED,
        nullptr);
    lv_label_set_long_mode(heading_, LV_LABEL_LONG_WRAP);
    const int tile_width = (UI_CONTENT_WIDTH - 2 * UI_MARGIN_X - 3 * UI_GUTTER) / 4;
    const int tile_y = UI_CONTENT_HEIGHT * 3 / 5;
    const int tile_height = UI_CONTENT_HEIGHT - tile_y - UI_PADDING_MEDIUM;
    midi_ = valueTileCreate(root_, UI_MARGIN_X, tile_y, tile_width, tile_height, "MIDI INPUT", "");
    level_ = valueTileCreate(root_,
                             UI_MARGIN_X + tile_width + UI_GUTTER,
                             tile_y,
                             tile_width,
                             tile_height,
                             "TRACK LEVEL",
                             "dB");
    pan_ = valueTileCreate(root_,
                           UI_MARGIN_X + 2 * (tile_width + UI_GUTTER),
                           tile_y,
                           tile_width,
                           tile_height,
                           "PAN / BALANCE",
                           "");
    mute_ = valueTileCreate(root_,
                            UI_MARGIN_X + 3 * (tile_width + UI_GUTTER),
                            tile_y,
                            tile_width,
                            tile_height,
                            "TRACK MUTE",
                            "");
    valueTileSetOnAdjust(mute_, [this](int delta) {
        focusControl(3);
        adjust(delta);
    });
    valueTileHideFill(mute_);
    valueTileHideFill(midi_);
    valueTileSetOnAdjust(midi_, [this](int delta) {
        focusControl(0);
        adjust(delta);
    });
    valueTileSetOnAdjust(level_, [this](int delta) {
        focusControl(1);
        adjust(delta);
    });
    valueTileSetOnAdjust(pan_, [this](int delta) {
        focusControl(2);
        adjust(delta);
    });
    for (auto* card: {midi_.card, level_.card, pan_.card, mute_.card})
        lv_obj_add_event_cb(card, controlEvent, LV_EVENT_PRESSED, this);
    status_ = lv_label_create(root_);
    ui_theme_apply_label_style(status_, false);
    lv_obj_set_style_text_font(status_, UI_FONT_SMALL, 0);
    lv_obj_set_pos(status_, UI_MARGIN_X, tile_y - 2 * UI_PADDING_MEDIUM);
    lv_obj_set_width(status_, UI_CONTENT_WIDTH - 2 * UI_MARGIN_X);
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
    midi_ = level_ = pan_ = mute_ = {};
    mix_pending_ = false;
    mix_.Reset(getCurrentTrack());
    pending_ = false;
    model_.Reset(getCurrentTrack());
}
void UITrackPage::onTrackChanged() {
    first_ = getCurrentTrack() < 8 ? 0 : 8;
    model_.Reset(getCurrentTrack());
    mix_.Reset(getCurrentTrack());
    mix_pending_ = false;
    alive_ = inter_mcu_backend_link_alive();
    pending_ = false;
    soft_state_ = UINT32_MAX;
    read();
    readMix();
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
    serviceMix();
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
void UITrackPage::setProgramChange(bool enabled) {
    if (!alive_ || !model_.Ready())
        return;
    if (inter_mcu_send_track_op(TRACK_OP_SET_PROGRAM_CHANGE, getCurrentTrack(), enabled) !=
        ESP_OK) {
        label(status_, "Link busy. Try again.");
        return;
    }
    model_.Reset(getCurrentTrack());
    pending_ = false;
    read();
    render();
}
void UITrackPage::focusControl(uint8_t control) {
    if (control_ == control)
        return;
    control_ = control;
    render();
}
void UITrackPage::controlEvent(lv_event_t* event) {
    auto* page = static_cast<UITrackPage*>(lv_event_get_user_data(event));
    auto* card = lv_event_get_current_target(event);
    page->focusControl(card == page->midi_.card
                           ? 0
                           : (card == page->level_.card ? 1 : (card == page->pan_.card ? 2 : 3)));
}
void UITrackPage::adjust(int delta) {
    if (control_ == 0) {
        if (model_.Ready())
            setMidi(TrackPageModel::StepMidi(model_.State().midi_in, delta));
    } else {
        adjustMix(control_ == 1   ? MIX_OP_SET_GAIN
                  : control_ == 2 ? MIX_OP_SET_PAN
                                  : MIX_OP_SET_MUTE,
                  delta);
    }
}
void UITrackPage::readMix() {
    if (!alive_)
        return;
    const MixStateRequest request{nextId(), getCurrentTrack()};
    mix_read_at_ = lv_tick_get();
    if (inter_mcu_request_mix_state(request) == ESP_OK) {
        mix_.Expect(request.request_id);
        mix_pending_ = true;
    }
}
void UITrackPage::serviceMix() {
    MixStateMessage state{};
    if (mix_pending_ && inter_mcu_get_mix_state(&state) && mix_.Accept(state)) {
        mix_pending_ = false;
        mix_accepted_at_ = lv_tick_get();
    }
    if (mix_.Valid() && lv_tick_elaps(mix_accepted_at_) > 2000)
        mix_.Reset(getCurrentTrack());
    if ((mix_pending_ && lv_tick_elaps(mix_read_at_) > 1500) ||
        (!mix_pending_ && lv_tick_elaps(mix_read_at_) > 500))
        readMix();
}
void UITrackPage::setMix(uint8_t op, uint16_t value) {
    if (!alive_ || !mix_.Ready())
        return;
    if (inter_mcu_set_track_mix(MixOpMessage{op, getCurrentTrack(), value}) != ESP_OK) {
        label(status_, "Link busy. Try again.");
        return;
    }
    mix_.Block();
    mix_pending_ = false;
    readMix();
    render();
}
void UITrackPage::adjustMix(uint8_t op, int delta) {
    if (!mix_.Ready())
        return;
    const auto& state = mix_.State();
    if (op == MIX_OP_SET_GAIN) {
        setMix(
            op,
            static_cast<uint16_t>(std::clamp(static_cast<int>(state.gain) + delta * 50, 0, 6600)));
    } else if (op == MIX_OP_SET_MUTE) {
        setMix(op, delta > 0 ? 1 : 0);
    } else {
        const int percent = static_cast<int>(std::lround(WaveX::Mix::WireToPan(state.pan) * 100));
        const int value = std::clamp(percent + delta, -100, 100);
        setMix(op, value == 0 ? 32768 : WaveX::Mix::PanToWire(static_cast<float>(value) / 100));
    }
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
                  "Tracks / Mixer - Track %u / %s",
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
    valueTileSetFocus(midi_, control_ == 0 && alive_ && model_.Ready());
    const auto& mix = mix_.State();
    if (!mix_.Valid())
        std::snprintf(text, sizeof(text), "--");
    else if (!mix.gain)
        std::snprintf(text, sizeof(text), "-inf");
    else
        std::snprintf(
            text, sizeof(text), "%.1f", static_cast<double>(WaveX::Mix::WireToGainDb(mix.gain)));
    valueTileSetValue(level_, text);
    valueTileSetFill(level_, mix_.Valid() ? static_cast<float>(mix.gain) / 6600 : 0);
    valueTileSetFocus(level_, control_ == 1 && alive_ && mix_.Ready());
    const int pan = static_cast<int>(std::lround(WaveX::Mix::WireToPan(mix.pan) * 100));
    if (!mix_.Valid())
        std::snprintf(text, sizeof(text), "--");
    else if (pan == 0)
        std::snprintf(text, sizeof(text), "Center");
    else
        std::snprintf(text, sizeof(text), "%s %d", pan < 0 ? "L" : "R", std::abs(pan));
    valueTileSetValue(pan_, text);
    valueTileSetFill(pan_, mix_.Valid() ? static_cast<float>(mix.pan) / 65535 : 0);
    valueTileSetFocus(pan_, control_ == 2 && alive_ && mix_.Ready());
    valueTileSetValue(mute_, !mix_.Valid() ? "--" : mix.mute ? "On" : "Off");
    valueTileSetFocus(mute_, control_ == 3 && alive_ && mix_.Ready());
    label(status_,
          !alive_ ? "Audio engine disconnected"
                  : (!model_.Valid()
                         ? "Reading Track settings..."
                         : (s.busy ? "Instrument loading..."
                                   : (!mix_.Valid() ? "Reading mixer settings..."
                                                    : "Assign loads an Instrument. Tap a control "
                                                      "or click encoder to focus."))));
    uint32_t state = (alive_ && model_.Ready() ? 1u : 0u) | (static_cast<uint32_t>(first_) << 1) |
                     (mix_.Ready() ? 64u : 0u) | (static_cast<uint32_t>(control_) << 7) |
                     (static_cast<uint32_t>(s.program_change) << 9);
    if (state != soft_state_) {
        soft_state_ = state;
        UINavigator::instance().refreshSoftkeys();
    }
}
std::array<Softkey, NUM_SOFTKEYS> UITrackPage::getSoftkeys() {
    std::array<Softkey, NUM_SOFTKEYS> k{};
    k[0] = {"Back", [] { UINavigator::instance().pop(); }};
    k[2] = {"Edit sound", [] { UINavigator::instance().jumpToRoot(RootGroup::Instrument); }};
    k[1] = {"Assign", [] {
                if (auto page = createInstrumentBrowserPage())
                    UINavigator::instance().push(page);
            }};
    const bool ready = alive_ && (control_ == 0 ? model_.Ready() : mix_.Ready());
    k[3] = {control_ == 0 ? "MIDI -"
                          : (control_ == 1   ? "Level -"
                             : control_ == 2 ? "Pan -"
                                             : "Unmute"),
            [this] { adjust(-1); },
            ready,
            "Reading settings"};
    k[4] = {control_ == 0 ? "MIDI +"
                          : (control_ == 1   ? "Level +"
                             : control_ == 2 ? "Pan +"
                                             : "Mute"),
            [this] { adjust(1); },
            ready,
            "Reading settings"};
    k[5] = {first_ ? "Tracks 1-8" : "Tracks 9-16", [this] {
                first_ = first_ ? 0 : 8;
                render();
            }};
    return k;
}
std::array<Softkey, NUM_SOFTKEYS> UITrackPage::getShiftedSoftkeys() {
    std::array<Softkey, NUM_SOFTKEYS> keys{};
    keys[0] = {"Back", [] { UINavigator::instance().pop(); }};
    keys[1] = {"Banks", [] { UINavigator::instance().push(createBankPage()); }};
    keys[2] = {"Project files", [] { UINavigator::instance().push(createProjectFilesPage()); }};
    keys[4] = {"Track poly", [] {
                   UINavigator::instance().push(createAllocationPage(WaveX::Protocol::ALLOC_TRACK));
               }};
    keys[5] = {"Sound poly", [] {
                   UINavigator::instance().push(createAllocationPage(WaveX::Protocol::ALLOC_SOUND));
               }};
    keys[3] = {model_.State().program_change ? "Program: On" : "Program: Off",
               [this] { setProgramChange(!model_.State().program_change); },
               alive_ && model_.Ready(),
               "Reading Track settings"};
    return keys;
}
void UITrackPage::onInput(const InputEvent& e) {
    if (e.type == InputType::EncoderRight || e.type == InputType::EncoderLeft)
        adjust(e.steps());
    else if (e.type == InputType::EncoderClick || e.type == InputType::ButtonPress)
        focusControl(static_cast<uint8_t>((control_ + 1) % 4));
}
size_t UITrackPage::consoleState(char* out, size_t cap, size_t len) {
    using namespace WaveX::Debug;
    len = AppendKvInt(out, cap, len, "trackready", alive_ && model_.Ready());
    len = AppendKvInt(out, cap, len, "trackfirst", first_ + 1);
    len = AppendKvInt(out, cap, len, "trackloaded", model_.Valid() && model_.State().loaded);
    len = AppendKvInt(out, cap, len, "mixready", alive_ && mix_.Ready());
    len = AppendKvInt(out, cap, len, "mixgain", mix_.State().gain);
    len = AppendKvInt(out, cap, len, "mixpan", mix_.State().pan);
    len = AppendKvInt(out, cap, len, "mixmute", mix_.State().mute);
    len = AppendKvInt(out, cap, len, "program", model_.State().program_change);
    return AppendKvInt(out, cap, len, "midiin", model_.State().midi_in);
}
bool UITrackPage::consoleCommand(const char* args, char* reply, size_t cap) {
    char field[12], extra;
    int value;
    if (!args || std::sscanf(args, "%11s %d %c", field, &value, &extra) != 2)
        return false;
    if (!std::strcmp(field, "SELECT") && value >= 1 && value <= 16)
        select(static_cast<uint8_t>(value - 1));
    else if (!std::strcmp(field, "MIDI") && value >= 0 && value <= 255 &&
             TrackMidiInValid(static_cast<uint8_t>(value)) && alive_ && model_.Ready())
        setMidi(static_cast<uint8_t>(value));
    else if (!std::strcmp(field, "PROGRAM") && value >= 0 && value <= 1 && alive_ && model_.Ready())
        setProgramChange(value != 0);
    else if (!std::strcmp(field, "LEVEL") && value >= 0 && value <= 6600 && alive_ && mix_.Ready())
        setMix(MIX_OP_SET_GAIN, static_cast<uint16_t>(value));
    else if (!std::strcmp(field, "PAN") && value >= 0 && value <= 65535 && alive_ && mix_.Ready())
        setMix(MIX_OP_SET_PAN, static_cast<uint16_t>(value));
    else if (!std::strcmp(field, "MUTE") && value >= 0 && value <= 1 && alive_ && mix_.Ready())
        setMix(MIX_OP_SET_MUTE, static_cast<uint16_t>(value));
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
