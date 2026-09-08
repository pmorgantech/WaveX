#include "ui/ui_sequencer_page.h"

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
constexpr int kGridX = UI_MARGIN_X + UI_SEQ_TRACK_WIDTH + UI_GUTTER;
constexpr int kGridW = UI_CONTENT_WIDTH - UI_MARGIN_X - kGridX;
constexpr int kCellW = (kGridW - 15 * UI_GUTTER) / 16;
constexpr const char* kScales[] = {"1/32", "1/16", "1/8", "1/4", "1/16T", "1/8T"};
// Owned by the serialized UI domain; survives page recreation.
uint32_t s_read_id = 0;

void text(lv_obj_t* label, const char* value) {
    if (label && std::strcmp(lv_label_get_text(label), value) != 0)
        lv_label_set_text(label, value);
}
void tileText(ValueTile& tile, const char* value) {
    if (tile.value && std::strcmp(lv_label_get_text(tile.value), value) != 0)
        valueTileSetValue(tile, value);
}
}  // namespace

void UISequencerPage::onEnter(lv_obj_t* parent) {
    root_ = lv_obj_create(parent);
    ui_theme_apply_container_style(root_, false);
    lv_obj_set_size(root_, UI_CONTENT_WIDTH, UI_CONTENT_HEIGHT);
    lv_obj_set_style_pad_all(root_, 0, 0);
    lv_obj_remove_flag(root_, LV_OBJ_FLAG_SCROLLABLE);
    const int width = (UI_CONTENT_WIDTH - 2 * UI_MARGIN_X - 3 * UI_GUTTER) / 4;
    const char* names[] = {"TEMPO", "SWING", "LENGTH", "SCALE", "VELOCITY", "PROBABILITY", "NOTE"};
    const char* units[] = {"BPM", "%", "steps", "", "", "%", "MIDI"};
    for (uint8_t i = 0; i < 7; ++i) {
        const int x = UI_MARGIN_X + (i < 4 ? i : i - 4) * (width + UI_GUTTER);
        const int y = i < 4 ? UI_PADDING_SMALL : UI_SEQ_DETAIL_TOP;
        tiles_[i] = valueTileCreate(root_,
                                    x,
                                    y,
                                    width,
                                    i < 4 ? UI_SEQ_TOOLBAR_HEIGHT : UI_SEQ_DETAIL_HEIGHT,
                                    names[i],
                                    units[i]);
        valueTileSetOnAdjust(tiles_[i], [this, i](int delta) { adjust(i, delta); });
        lv_obj_add_flag(tiles_[i].bar_track, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(tiles_[i].knob, LV_OBJ_FLAG_HIDDEN);
    }
    status_ = lv_label_create(root_);
    ui_theme_apply_label_style(status_, false);
    lv_obj_set_pos(status_, UI_MARGIN_X + 3 * (width + UI_GUTTER), UI_SEQ_DETAIL_TOP);
    lv_obj_set_width(status_, width);
    lv_obj_set_style_text_font(status_, UI_FONT_SMALL, 0);
    lv_label_set_long_mode(status_, LV_LABEL_LONG_WRAP);
    for (uint8_t row = 0; row < 4; ++row) {
        const int y = UI_SEQ_GRID_TOP + row * UI_SEQ_ROW_HEIGHT;
        auto* button = lv_button_create(root_);
        row_buttons_[row] = button;
        ui_theme_apply_button_style(button, false);
        lv_obj_set_pos(button, UI_MARGIN_X, y);
        lv_obj_set_size(button, UI_SEQ_TRACK_WIDTH, UI_SEQ_ROW_HEIGHT - UI_GUTTER);
        row_labels_[row] = lv_label_create(button);
        lv_obj_set_width(row_labels_[row], UI_SEQ_TRACK_WIDTH - 2 * UI_PADDING_MEDIUM);
        lv_obj_set_style_text_font(row_labels_[row], UI_FONT_SMALL, 0);
        lv_label_set_long_mode(row_labels_[row], LV_LABEL_LONG_DOT);
        lv_obj_center(row_labels_[row]);
        row_context_[row].owner = this;
        row_context_[row].row = row;
        lv_obj_add_event_cb(button, rowEvent, LV_EVENT_CLICKED, &row_context_[row]);
        for (uint8_t col = 0; col < 16; ++col) {
            auto& cell = cells_[row][col];
            cell.owner = this;
            cell.row = row;
            cell.column = col;
            cell.drawn = UINT32_MAX;
            cell.button = lv_button_create(root_);
            ui_theme_apply_button_style(cell.button, false);
            lv_obj_set_pos(cell.button, kGridX + col * (kCellW + UI_GUTTER), y);
            lv_obj_set_size(cell.button, kCellW, UI_SEQ_ROW_HEIGHT - UI_GUTTER);
            lv_obj_set_style_pad_all(cell.button, 0, 0);
            cell.label = lv_label_create(cell.button);
            lv_obj_set_style_text_font(cell.label, UI_FONT_MONO_SMALL, 0);
            lv_obj_center(cell.label);
            lv_obj_add_event_cb(cell.button, cellEvent, LV_EVENT_CLICKED, &cell);
        }
    }
    settings_.valid = 0;
    link_alive_ = inter_mcu_backend_link_alive();
    window(static_cast<uint8_t>((getCurrentTrack() / 4) * 4),
           static_cast<uint8_t>((selected_step_ / 16) * 16));
    inter_mcu_request_track_binding(0xFF);
    timer_ = lv_timer_create(timerEvent, 40, this);
    service();
}

void UISequencerPage::onExit() {
    if (timer_) {
        lv_timer_delete(timer_);
        timer_ = nullptr;
    }
    if (root_) {
        lv_obj_delete(root_);
        root_ = nullptr;
    }
    for (auto& row: cells_)
        for (auto& cell: row)
            cell = Cell{};
    for (auto& tile: tiles_)
        tile = ValueTile{};
    for (auto& label: row_labels_)
        label = nullptr;
    for (auto& button: row_buttons_)
        button = nullptr;
    status_ = nullptr;
    model_.Invalidate();
    clear_armed_ = false;
    // Navigation does not stop an ongoing pattern.
}

uint8_t UISequencerPage::selectedRow() const {
    return static_cast<uint8_t>(getCurrentTrack() - model_.FirstTrack());
}
bool UISequencerPage::editable() const {
    return link_alive_ && model_.Ready(selectedRow()) && selected_step_ < settings_.length;
}
bool UISequencerPage::valueStep(SequencerGridModel::Step& step) const {
    return link_alive_ && settings_.valid && selected_step_ < settings_.length &&
           model_.CopyStepForEdit(selectedRow(), selected_step_ % 16, step);
}
void UISequencerPage::requestRow(uint8_t row) {
    if (!link_alive_ || row >= 4)
        return;
    if (++s_read_id == 0)
        ++s_read_id;
    const auto request = model_.BeginRead(s_read_id, row);
    next_row_ = row;
    requested_at_ = lv_tick_get();
    inter_mcu_request_seq_page(request);  // timeout path retries a fresh id
}
void UISequencerPage::window(uint8_t track, uint8_t step) {
    if (!model_.SetWindow(track, step))
        return;
    clear_armed_ = false;
    next_row_ = 0;
    requested_at_ = lv_tick_get();
    requestRow(0);
    render();
    UINavigator::instance().refreshSoftkeys();
}
void UISequencerPage::focus(uint8_t track, uint8_t step) {
    if (track >= SEQ_TRACK_COUNT || step >= SEQ_MAX_STEPS)
        return;
    if (getCurrentTrack() != track || selected_step_ != step)
        model_.DiscardPreview();
    setCurrentTrack(track);
    selected_step_ = step;
    const uint8_t first_track = static_cast<uint8_t>((track / 4) * 4);
    const uint8_t first_step = static_cast<uint8_t>((step / 16) * 16);
    if (model_.FirstTrack() != first_track || model_.FirstStep() != first_step)
        window(first_track, first_step);
    clear_armed_ = false;
    render();
    UINavigator::instance().refreshSoftkeys();
    UINavigator::instance().refreshContext();
}
void UISequencerPage::onTrackChanged() {
    focus(getCurrentTrack(), selected_step_);
}

void UISequencerPage::service() {
    const bool alive = inter_mcu_backend_link_alive();
    if (alive != link_alive_) {
        link_alive_ = alive;
        model_.Invalidate();
        settings_.valid = 0;
        if (alive)
            requestRow(0);
        UINavigator::instance().refreshSoftkeys();
    }
    if (alive) {
        SeqPatternSyncMessage page;
        const bool was_ready = model_.AllReady();
        if (inter_mcu_get_seq_page(&page) && model_.Accept(page)) {
            settings_ = page;
            next_row_ = static_cast<uint8_t>((page.track - model_.FirstTrack() + 1) % 4);
            if (was_ready != model_.AllReady())
                UINavigator::instance().refreshSoftkeys();
        }
        SeqPlayheadMessage head;
        if (inter_mcu_get_seq_playhead(&head) && head.step < SEQ_MAX_STEPS && head.playing <= 1) {
            const bool changed = head.playing != playhead_.playing;
            playhead_ = head;
            if (changed)
                UINavigator::instance().refreshSoftkeys();
        }
        const auto elapsed = static_cast<uint32_t>(lv_tick_get() - requested_at_);
        if ((!model_.Waiting() && elapsed >= 80) || elapsed >= 600)
            requestRow(next_row_);
    }
    render();
}

void UISequencerPage::render() {
    if (!root_)
        return;
    char value[192];
    std::snprintf(value,
                  sizeof(value),
                  "%u.%02u",
                  settings_.tempo_bpm_x100 / 100,
                  settings_.tempo_bpm_x100 % 100);
    tileText(tiles_[0], value);
    std::snprintf(value, sizeof(value), "%u", settings_.swing);
    tileText(tiles_[1], value);
    std::snprintf(value, sizeof(value), "%u", settings_.length);
    tileText(tiles_[2], value);
    tileText(tiles_[3], settings_.scale < 6 ? kScales[settings_.scale] : "--");
    if (!settings_.valid)
        for (uint8_t i = 0; i < 4; ++i)
            tileText(tiles_[i], "--");
    for (uint8_t row = 0; row < 4; ++row) {
        const uint8_t track = model_.FirstTrack() + row;
        TrackBindingMessage binding;
        const bool bound = inter_mcu_get_track_binding(track, &binding);
        std::snprintf(value,
                      sizeof(value),
                      "Track %u%s\n%s",
                      trackDisplayNumber(track),
                      model_.Ready(row) && !model_.Row(row).enabled ? " / MUTE" : "",
                      bound && binding.name[0] ? binding.name : "Empty");
        text(row_labels_[row], value);
        lv_obj_set_style_bg_color(
            row_buttons_[row], getCurrentTrack() == track ? UI_COLOR_ACCENT : UI_COLOR_CARD_ALT, 0);
        lv_obj_set_style_border_color(
            row_buttons_[row], getCurrentTrack() == track ? UI_COLOR_ACCENT : UI_COLOR_LINE, 0);
        for (uint8_t col = 0; col < 16; ++col) {
            auto& cell = cells_[row][col];
            const uint8_t step = model_.FirstStep() + col;
            const bool ready = link_alive_ && model_.Ready(row);
            const bool enabled = ready && step < model_.Row(row).length;
            const bool on = ready && model_.Row(row).steps[col].on;
            const bool head = link_alive_ && playhead_.playing && playhead_.step == step;
            const bool selected = getCurrentTrack() == track && selected_step_ == step;
            const uint32_t flags =
                (enabled ? 1u : 0u) | (on ? 2u : 0u) | (head ? 4u : 0u) | (selected ? 8u : 0u);
            if (flags != cell.drawn) {
                if (enabled)
                    lv_obj_remove_state(cell.button, LV_STATE_DISABLED);
                else
                    lv_obj_add_state(cell.button, LV_STATE_DISABLED);
                lv_obj_set_style_bg_color(cell.button, on ? UI_COLOR_ACCENT : UI_COLOR_CARD_ALT, 0);
                lv_obj_set_style_text_color(cell.button, on ? UI_COLOR_ACCENT_FG : UI_COLOR_DIM, 0);
                lv_obj_set_style_border_width(
                    cell.button, head || selected ? UI_BORDER_WIDTH_FOCUS : UI_BORDER_WIDTH, 0);
                lv_obj_set_style_border_color(cell.button,
                                              head       ? UI_COLOR_WARN
                                              : selected ? UI_COLOR_ACCENT
                                                         : UI_COLOR_LINE,
                                              0);
                cell.drawn = flags;
            }
            std::snprintf(value, sizeof(value), "%02u", step + 1);
            text(cell.label, value);
        }
    }
    SequencerGridModel::Step step;
    if (valueStep(step)) {
        std::snprintf(value, sizeof(value), "%u", step.velocity);
        tileText(tiles_[4], value);
        std::snprintf(value, sizeof(value), "%u", step.probability);
        tileText(tiles_[5], value);
        std::snprintf(value, sizeof(value), "%u", step.note);
        tileText(tiles_[6], value);
        if (step.note >= 60 && step.note < 76)
            std::snprintf(value, sizeof(value), "Pad %u", step.note - 59);
        else
            std::snprintf(value, sizeof(value), "MIDI");
        text(tiles_[6].unit, value);
    } else {
        tileText(tiles_[4], "--");
        tileText(tiles_[5], "--");
        tileText(tiles_[6], "--");
        text(tiles_[6].unit, "MIDI");
    }
    std::snprintf(value,
                  sizeof(value),
                  "Track %u / Step %02u\n%s",
                  trackDisplayNumber(getCurrentTrack()),
                  selected_step_ + 1,
                  !link_alive_         ? "Audio engine disconnected"
                  : clear_armed_       ? "Clear this Track's steps? Choose Confirm or Cancel."
                  : !model_.AllReady() ? "Reading pattern..."
                                       : "Tap steps; drag values. Notes 60-75 play Pads 1-16.");
    text(status_, value);
    char context[sizeof(context_)];
    std::snprintf(context,
                  sizeof(context),
                  "Pattern 1 / Steps %u-%u / %s",
                  model_.FirstStep() + 1,
                  model_.FirstStep() + 16,
                  !link_alive_        ? "Disconnected"
                  : playhead_.playing ? "Playing"
                                      : "Stopped");
    if (std::strcmp(context, context_) != 0) {
        std::snprintf(context_, sizeof(context_), "%s", context);
        UINavigator::instance().refreshContext();
    }
}

bool UISequencerPage::edit(const SeqPatternOpMessage& message) {
    if (!link_alive_)
        return false;
    if (inter_mcu_send_seq_pattern_op(message) != ESP_OK)
        return false;
    const uint8_t row = static_cast<uint8_t>(message.track - model_.FirstTrack());
    if (message.op == SEQ_OP_PATTERN_LENGTH || message.op == SEQ_OP_PATTERN_SCALE ||
        message.op == SEQ_OP_PATTERN_SWING) {
        model_.Invalidate();
        requestRow(0);
    } else {
        model_.InvalidateRow(row);
        requestRow(row);
    }
    render();
    UINavigator::instance().refreshSoftkeys();
    return true;
}

void UISequencerPage::toggle(uint8_t row, uint8_t column) {
    if (row >= 4 || column >= 16 || !model_.Ready(row) || !link_alive_)
        return;
    const uint8_t step = model_.FirstStep() + column;
    if (step >= model_.Row(row).length)
        return;
    const auto saved = model_.Row(row).steps[column];
    focus(model_.FirstTrack() + row, step);
    edit({SEQ_OP_SET_STEP,
          getCurrentTrack(),
          step,
          static_cast<uint8_t>(!saved.on),
          saved.velocity,
          0});
}

void UISequencerPage::adjust(uint8_t parameter, int delta) {
    if (!link_alive_ || !settings_.valid || delta == 0)
        return;
    if (parameter == 0) {
        const int bpm =
            std::clamp(static_cast<int>(settings_.tempo_bpm_x100) + delta * 100, 2000, 30000);
        if (inter_mcu_send_seq_transport({SEQ_TRANSPORT_CONFIGURE,
                                          settings_.clock_source,
                                          settings_.input_mode,
                                          settings_.quantize,
                                          static_cast<uint16_t>(bpm),
                                          0}) != ESP_OK)
            return;
        settings_.tempo_bpm_x100 = static_cast<uint16_t>(bpm);
        model_.Invalidate();
        requestRow(0);
    } else if (parameter <= 3) {
        const int old = parameter == 1   ? settings_.swing
                        : parameter == 2 ? settings_.length
                                         : settings_.scale;
        const int next = std::clamp(old + delta,
                                    parameter == 1   ? 50
                                    : parameter == 2 ? 1
                                                     : 0,
                                    parameter == 1   ? 75
                                    : parameter == 2 ? 64
                                                     : 5);
        if (edit({static_cast<uint8_t>(parameter == 1   ? SEQ_OP_PATTERN_SWING
                                       : parameter == 2 ? SEQ_OP_PATTERN_LENGTH
                                                        : SEQ_OP_PATTERN_SCALE),
                  0,
                  0,
                  static_cast<uint8_t>(next),
                  static_cast<uint16_t>(next),
                  0})) {
            if (parameter == 1)
                settings_.swing = static_cast<uint8_t>(next);
            else if (parameter == 2)
                settings_.length = static_cast<uint8_t>(next);
            else
                settings_.scale = static_cast<uint8_t>(next);
        }
    } else {
        SequencerGridModel::Step saved;
        if (!valueStep(saved))
            return;
        bool sent = false;
        if (parameter == 4) {
            saved.velocity = static_cast<uint8_t>(std::clamp(saved.velocity + delta, 1, 127));
            sent = edit(
                {SEQ_OP_SET_STEP, getCurrentTrack(), selected_step_, saved.on, saved.velocity, 0});
        } else if (parameter == 5) {
            saved.probability = static_cast<uint8_t>(std::clamp(saved.probability + delta, 0, 100));
            sent = edit(
                {SEQ_OP_SET_STEP_PROB, getCurrentTrack(), selected_step_, saved.probability, 0, 0});
        }
        if (parameter == 6) {
            saved.note = static_cast<uint8_t>(std::clamp(saved.note + delta, 0, 127));
            sent =
                edit({SEQ_OP_SET_STEP_NOTE, getCurrentTrack(), selected_step_, saved.note, 0, 0});
        }
        if (sent) {
            model_.PreviewStep(selectedRow(), selected_step_ % 16, saved);
            render();
        }
    }
}
void UISequencerPage::transport() {
    if (!link_alive_ || !model_.AllReady())
        return;
    inter_mcu_send_seq_transport(
        {static_cast<uint8_t>(playhead_.playing ? SEQ_TRANSPORT_STOP : SEQ_TRANSPORT_PLAY),
         settings_.clock_source,
         settings_.input_mode,
         settings_.quantize,
         settings_.tempo_bpm_x100,
         0});
}
void UISequencerPage::clearRow() {
    clear_armed_ = false;
    edit({SEQ_OP_CLEAR_TRACK, getCurrentTrack(), 0, 0, 0, 0});
}
void UISequencerPage::cellEvent(lv_event_t* event) {
    auto* cell = static_cast<Cell*>(lv_event_get_user_data(event));
    cell->owner->toggle(cell->row, cell->column);
}
void UISequencerPage::rowEvent(lv_event_t* event) {
    auto* row = static_cast<Cell*>(lv_event_get_user_data(event));
    row->owner->focus(row->owner->model_.FirstTrack() + row->row, row->owner->selected_step_);
}
void UISequencerPage::timerEvent(lv_timer_t* timer) {
    static_cast<UISequencerPage*>(lv_timer_get_user_data(timer))->service();
}
void UISequencerPage::onInput(const InputEvent& event) {
    if (event.type == InputType::EncoderLeft || event.type == InputType::EncoderRight ||
        event.type == InputType::EncoderUp || event.type == InputType::EncoderDown)
        focus(getCurrentTrack(),
              static_cast<uint8_t>(std::clamp(selected_step_ + event.steps(), 0, 63)));
    else if (event.type == InputType::ButtonPress || event.type == InputType::EncoderClick)
        toggle(selectedRow(), selected_step_ % 16);
}
std::array<Softkey, NUM_SOFTKEYS> UISequencerPage::getSoftkeys() {
    std::array<Softkey, NUM_SOFTKEYS> keys{};
    keys[0] = {"Back", [] { UINavigator::instance().pop(); }};
    const bool ready = link_alive_ && model_.AllReady();
    if (clear_armed_) {
        keys[1] = {"Cancel", [this] {
                       clear_armed_ = false;
                       render();
                       UINavigator::instance().refreshSoftkeys();
                   }};
        keys[2] = {"Confirm", [this] { clearRow(); }, ready, "Waiting for the audio engine"};
        return keys;
    }
    keys[1] = {playhead_.playing ? "Stop" : "Play",
               [this] { transport(); },
               ready,
               "Waiting for the audio engine"};
    keys[2] = {"Tracks -",
               [this] { focus(getCurrentTrack() - 4, selected_step_); },
               model_.FirstTrack() > 0,
               "First four Tracks"};
    keys[3] = {"Tracks +",
               [this] { focus(getCurrentTrack() + 4, selected_step_); },
               model_.FirstTrack() < 12,
               "Last four Tracks"};
    keys[4] = {"Steps -",
               [this] { focus(getCurrentTrack(), selected_step_ - 16); },
               model_.FirstStep() > 0,
               "First sixteen steps"};
    keys[5] = {"Steps +",
               [this] { focus(getCurrentTrack(), selected_step_ + 16); },
               model_.FirstStep() < 48,
               "Last sixteen steps"};
    return keys;
}
std::array<Softkey, NUM_SOFTKEYS> UISequencerPage::getShiftedSoftkeys() {
    std::array<Softkey, NUM_SOFTKEYS> keys{};
    keys[0] = {"Back", [] { UINavigator::instance().pop(); }};
    const bool ready = link_alive_ && model_.Ready(selectedRow());
    keys[1] = {ready && !model_.Row(selectedRow()).enabled ? "Unmute" : "Mute",
               [this] {
                   if (model_.Ready(selectedRow()))
                       edit({SEQ_OP_TRACK_MUTE,
                             getCurrentTrack(),
                             0,
                             static_cast<uint8_t>(!model_.Row(selectedRow()).enabled),
                             0,
                             0});
               },
               ready,
               "Waiting for the Track"};
    keys[2] = {"Clear row",
               [this] {
                   clear_armed_ = true;
                   render();
                   UINavigator::instance().refreshSoftkeys();
               },
               ready,
               "Waiting for the Track"};
    keys[3] = {"Step off",
               [this] {
                   if (editable())
                       edit({SEQ_OP_SET_STEP,
                             getCurrentTrack(),
                             selected_step_,
                             0,
                             model_.Row(selectedRow()).steps[selected_step_ % 16].velocity,
                             0});
               },
               editable(),
               "Select an active step window"};
    return keys;
}
size_t UISequencerPage::consoleState(char* out, size_t cap, size_t len) {
    using namespace WaveX::Debug;
    len = AppendKvInt(out, cap, len, "seqready", model_.AllReady() && link_alive_);
    len = AppendKvInt(out, cap, len, "seqplaying", playhead_.playing);
    len = AppendKvInt(out, cap, len, "seqstep", playhead_.step + 1);
    len = AppendKvInt(out, cap, len, "seqtrack", trackDisplayNumber(getCurrentTrack()));
    len = AppendKvInt(out, cap, len, "seqpage", model_.FirstStep() + 1);
    len = AppendKvInt(out, cap, len, "seqlen", settings_.length);
    len = AppendKvInt(out, cap, len, "seqtempo", settings_.tempo_bpm_x100);
    len = AppendKvInt(out, cap, len, "seqswing", settings_.swing);
    uint16_t bits = 0;
    if (model_.Ready(selectedRow())) {
        const auto& page = model_.Row(selectedRow());
        for (uint8_t i = 0; i < 16; ++i)
            if (page.steps[i].on)
                bits |= static_cast<uint16_t>(1u << i);
        len = AppendKvInt(out, cap, len, "seqnote", page.steps[selected_step_ % 16].note);
        len = AppendKvInt(out, cap, len, "seqvel", page.steps[selected_step_ % 16].velocity);
        len = AppendKvInt(out, cap, len, "seqprob", page.steps[selected_step_ % 16].probability);
        len = AppendKvInt(out, cap, len, "seqmuted", !page.enabled);
    }
    return AppendKvInt(out, cap, len, "seqbits", bits);
}
bool UISequencerPage::consoleCommand(const char* args, char* reply, size_t cap) {
    char verb[24]{};
    int a = 0, b = 0;
    SequencerGridModel::Step step;
    const int count = std::sscanf(args ? args : "", "%23s %d %d", verb, &a, &b);
    if (count == 3 && std::strcmp(verb, "FOCUS") == 0 && a >= 1 && a <= 16 && b >= 1 && b <= 64)
        focus(static_cast<uint8_t>(a - 1), static_cast<uint8_t>(b - 1));
    else if (count == 1 && std::strcmp(verb, "TOGGLE") == 0 && editable())
        lv_obj_send_event(
            cells_[selectedRow()][selected_step_ % 16].button, LV_EVENT_CLICKED, nullptr);
    else if (count == 2 && link_alive_ && settings_.valid) {
        if (std::strcmp(verb, "TEMPO") == 0 && a >= 2000 && a <= 30000 && a % 100 == 0)
            adjust(0, (a - settings_.tempo_bpm_x100) / 100);
        else if (std::strcmp(verb, "SWING") == 0 && a >= 50 && a <= 75)
            adjust(1, a - settings_.swing);
        else if (std::strcmp(verb, "LENGTH") == 0 && a >= 1 && a <= 64)
            adjust(2, a - settings_.length);
        else if (std::strcmp(verb, "NOTE") == 0 && a >= 0 && a <= 127 && valueStep(step))
            adjust(6, a - step.note);
        else if (std::strcmp(verb, "VELOCITY") == 0 && a >= 1 && a <= 127 && valueStep(step))
            adjust(4, a - step.velocity);
        else if (std::strcmp(verb, "PROBABILITY") == 0 && a >= 0 && a <= 100 && valueStep(step))
            adjust(5, a - step.probability);
        else
            return false;
    } else
        return false;
    std::snprintf(reply, cap, "ok");
    return true;
}
std::shared_ptr<UIPage> createSequencerPage() {
    return std::make_shared<UISequencerPage>();
}
}  // namespace wavex_ui
