#include "ui/ui_notes_page.h"

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
    if (!++id)
        ++id;
    return id;
}
void text(lv_obj_t* label, const char* value) {
    if (std::strcmp(lv_label_get_text(label), value))
        lv_label_set_text(label, value);
}
const char* modes[] = {"Play", "Step rec", "Live rec", "Erase"};
const char* mode_keys[] = {"Mode: Play", "Mode: Step", "Mode: Live", "Mode: Erase"};
const char* quantize_names[] = {"off", "step", "half-step"};
const char* quantize_keys[] = {"Quant: Off", "Quant: Step", "Quant: Half"};
}  // namespace
void UINotesPage::onEnter(lv_obj_t* parent) {
    root_ = lv_obj_create(parent);
    ui_theme_apply_container_style(root_, false);
    lv_obj_set_size(root_, UI_CONTENT_WIDTH, UI_CONTENT_HEIGHT);
    lv_obj_set_style_pad_all(root_, 0, 0);
    lv_obj_remove_flag(root_, LV_OBJ_FLAG_SCROLLABLE);
    const int width = (UI_CONTENT_WIDTH - 2 * UI_MARGIN_X - 3 * UI_GUTTER) / 4;
    const int height = UI_CONTENT_HEIGHT / 2;
    const char* names[] = {"LANE", "NOTE", "VELOCITY", "GATE TICKS"};
    for (uint8_t i = 0; i < 4; ++i) {
        tiles_[i] = valueTileCreate(root_,
                                    UI_MARGIN_X + i * (width + UI_GUTTER),
                                    UI_PADDING_LARGE,
                                    width,
                                    height,
                                    names[i],
                                    "");
        lv_obj_add_flag(tiles_[i].bar_track, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(tiles_[i].knob, LV_OBJ_FLAG_HIDDEN);
        valueTileSetOnAdjust(tiles_[i], [this, i](int delta) {
            focus_ = i;
            adjust(i, delta);
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
void UINotesPage::onExit() {
    if (timer_)
        lv_timer_delete(timer_);
    timer_ = nullptr;
    if (root_)
        lv_obj_delete(root_);
    root_ = status_ = nullptr;
    ready_ = false;
}
void UINotesPage::onTrackChanged() {
    ready_ = false;
    feedback_ = false;
    arm_target_ = true;
    read();
    render();
}
void UINotesPage::read() {
    if (!inter_mcu_backend_link_alive())
        return;
    const auto id = nextId();
    if (inter_mcu_request_seq_notes({id, getCurrentTrack(), step_, 0}) == ESP_OK)
        expected_ = id;
    read_at_ = lv_tick_get();
}
void UINotesPage::service() {
    SeqNotesMessage reply;
    if (!inter_mcu_backend_link_alive()) {
        ready_ = false;
        arm_target_ = true;
    } else if (inter_mcu_get_seq_notes(&reply) && IsValidSeqNotes(reply) &&
               reply.request_id == expected_ && reply.track == getCurrentTrack() &&
               reply.step == step_) {
        const bool changed =
            state_.epoch && (state_.epoch != reply.epoch || state_.pattern != reply.pattern);
        if (!changed && state_.epoch && state_.track == reply.track && state_.step == reply.step &&
            reply.input_mode && std::memcmp(state_.notes, reply.notes, sizeof(reply.notes))) {
            feedback_ = true;
            feedback_at_ = lv_tick_get();
        } else if (changed) {
            feedback_ = false;
        }
        state_ = reply;
        ready_ = true;
        expected_ = 0;
        if (changed)
            arm_target_ = true;
        if (arm_target_ && !state_.read_only) {
            arm_target_ = false;
            edit({SEQ_OP_RECORD_TARGET, getCurrentTrack(), step_, 0, 0, 0});
        }
    }
    if (static_cast<uint32_t>(lv_tick_get() - read_at_) >= 300)
        read();
    render();
}
bool UINotesPage::edit(const SeqPatternOpMessage& op) {
    if (!ready_ || state_.read_only || !inter_mcu_backend_link_alive())
        return false;
    SeqSlotEditMessage message;
    message.epoch = state_.epoch;
    message.pattern = state_.pattern;
    message.edit = op;
    if (inter_mcu_send_seq_slot_edit(message) != ESP_OK)
        return false;
    ready_ = false;
    read();
    render();
    return true;
}
bool UINotesPage::lane(SeqNoteLaneState n) {
    return edit({SEQ_OP_SET_NOTE_LANE,
                 getCurrentTrack(),
                 step_,
                 lane_,
                 static_cast<uint16_t>(n.note | (n.velocity << 8)),
                 static_cast<int16_t>(n.gate_ticks)});
}
void UINotesPage::adjust(uint8_t field, int delta) {
    if (!delta)
        return;
    if (field == 0) {
        lane_ = static_cast<uint8_t>(std::clamp(static_cast<int>(lane_) + delta, 0, 3));
        render();
        return;
    }
    if (!ready_)
        return;
    auto n = state_.notes[lane_];
    if (field == 1)
        n.note = static_cast<uint8_t>(std::clamp(static_cast<int>(n.note) + delta, 0, 127));
    if (field == 2)
        n.velocity = static_cast<uint8_t>(std::clamp(static_cast<int>(n.velocity) + delta, 0, 127));
    if (field == 3)
        n.gate_ticks =
            static_cast<uint16_t>(std::clamp(static_cast<int>(n.gate_ticks) + delta, 0, 32767));
    lane(n);
}
void UINotesPage::selectStep(int delta) {
    step_ = static_cast<uint8_t>(std::clamp(static_cast<int>(step_) + delta, 0, 63));
    ready_ = false;
    feedback_ = false;
    arm_target_ = true;
    read();
    render();
}
void UINotesPage::transport(uint8_t command, uint8_t mode, uint8_t quantize) {
    if (!ready_ || state_.read_only)
        return;
    if (inter_mcu_send_seq_transport(
            {command, state_.clock_source, mode, quantize, state_.tempo_bpm_x100, 0}) == ESP_OK) {
        ready_ = false;
        read();
        render();
    }
}
void UINotesPage::render() {
    if (!root_)
        return;
    if (feedback_ && static_cast<uint32_t>(lv_tick_get() - feedback_at_) >= 500)
        feedback_ = false;
    char values[4][24];
    const auto& n = state_.notes[lane_];
    std::snprintf(values[0], 24, "%u / 4", lane_ + 1);
    std::snprintf(values[1], 24, "%u", n.note);
    std::snprintf(values[2], 24, "%u", n.velocity);
    if (n.gate_ticks)
        std::snprintf(values[3], 24, "%u", n.gate_ticks);
    else
        std::snprintf(values[3], 24, "Hold");
    for (uint8_t i = 0; i < 4; ++i) {
        text(tiles_[i].value, state_.epoch ? values[i] : "--");
        valueTileSetFocus(tiles_[i], focus_ == i && ready_);
    }
    char status[600];
    std::snprintf(status,
                  sizeof(status),
                  "Track %u / Step %u / %s / %s / Quantize %s%s%s\n"
                  "Lanes: %u (%u)   %u (%u)   %u (%u)   %u (%u)\n"
                  "Velocity 0 clears a lane. 96 ticks = one quarter note; gate 0 holds until "
                  "retrigger or Stop.\n"
                  "Record target: Track %u, Step %u. Step record advances after all keys lift.\n"
                  "Shift: drum/melodic, quantize, clear lane. Save with Pattern or Project.",
                  getCurrentTrack() + 1,
                  step_ + 1,
                  state_.melodic ? "Melodic" : "Drum",
                  modes[state_.input_mode],
                  quantize_names[state_.quantize],
                  state_.read_only ? " / Song read-only"
                  : ready_         ? ""
                                   : " / Reading...",
                  feedback_ ? " / Step updated" : "",
                  state_.notes[0].note,
                  state_.notes[0].velocity,
                  state_.notes[1].note,
                  state_.notes[1].velocity,
                  state_.notes[2].note,
                  state_.notes[2].velocity,
                  state_.notes[3].note,
                  state_.notes[3].velocity,
                  getCurrentTrack() + 1,
                  state_.record_step + 1);
    text(status_, status);
    const uint32_t keys = ready_ | (state_.read_only << 1) | (state_.melodic << 2) |
                          (state_.playing << 3) | (state_.quantize << 4) |
                          (state_.input_mode << 6) | (step_ << 8) | (lane_ << 16) | (focus_ << 20);
    if (keys != keys_) {
        keys_ = keys;
        UINavigator::instance().refreshSoftkeys();
    }
}
std::array<Softkey, NUM_SOFTKEYS> UINotesPage::getSoftkeys() {
    std::array<Softkey, NUM_SOFTKEYS> k{};
    k[0] = {"Back", [] { UINavigator::instance().pop(); }};
    const bool enabled = ready_ && !state_.read_only;
    k[1] = {state_.playing ? "Stop" : "Play",
            [this] {
                transport(state_.playing ? SEQ_TRANSPORT_STOP : SEQ_TRANSPORT_PLAY,
                          state_.input_mode,
                          state_.quantize);
            },
            enabled,
            "Reading step"};
    k[2] = {"Step -", [this] { selectStep(-1); }, step_ > 0, "First step"};
    k[3] = {"Step +", [this] { selectStep(1); }, step_ < 63, "Last step"};
    k[4] = {"Field >", [this] {
                focus_ = static_cast<uint8_t>((focus_ + 1) % 4);
                render();
            }};
    k[5] = {mode_keys[state_.input_mode],
            [this] {
                transport(SEQ_TRANSPORT_CONFIGURE,
                          static_cast<uint8_t>((state_.input_mode + 1) % 4),
                          state_.quantize);
            },
            enabled,
            "Reading step"};
    return k;
}
std::array<Softkey, NUM_SOFTKEYS> UINotesPage::getShiftedSoftkeys() {
    auto k = getSoftkeys();
    const bool enabled = ready_ && !state_.read_only;
    k[1] = {state_.melodic ? "To drum" : "To melody",
            [this] {
                edit({SEQ_OP_SET_MELODIC,
                      getCurrentTrack(),
                      step_,
                      static_cast<uint8_t>(!state_.melodic),
                      0,
                      0});
            },
            enabled,
            "Reading step"};
    k[2] = {quantize_keys[state_.quantize],
            [this] {
                transport(SEQ_TRANSPORT_CONFIGURE,
                          state_.input_mode,
                          static_cast<uint8_t>((state_.quantize + 1) % 3));
            },
            enabled,
            "Reading step"};
    k[3] = {"Clear lane",
            [this] {
                auto n = state_.notes[lane_];
                n.velocity = 0;
                lane(n);
            },
            enabled,
            "Reading step"};
    k[4] = {"Lane -", [this] { adjust(0, -1); }, lane_ > 0, "First lane"};
    k[5] = {"Lane +", [this] { adjust(0, 1); }, lane_ < 3, "Last lane"};
    return k;
}
void UINotesPage::onInput(const InputEvent& e) {
    if (e.type == InputType::EncoderRight || e.type == InputType::EncoderLeft)
        adjust(focus_, e.steps());
    else if (e.type == InputType::EncoderClick) {
        focus_ = static_cast<uint8_t>((focus_ + 1) % 4);
        render();
    }
}
size_t UINotesPage::consoleState(char* out, size_t cap, size_t len) {
    using namespace WaveX::Debug;
    len = AppendKvInt(out, cap, len, "notesready", ready_);
    len = AppendKvInt(out, cap, len, "melodic", state_.melodic);
    len = AppendKvInt(out, cap, len, "step", step_);
    len = AppendKvInt(out, cap, len, "lane", lane_);
    len = AppendKvInt(out, cap, len, "note", state_.notes[lane_].note);
    len = AppendKvInt(out, cap, len, "velocity", state_.notes[lane_].velocity);
    len = AppendKvInt(out, cap, len, "gate", state_.notes[lane_].gate_ticks);
    len = AppendKvInt(out, cap, len, "record", state_.input_mode);
    len = AppendKvInt(out, cap, len, "quantize", state_.quantize);
    len = AppendKvInt(out, cap, len, "feedback", feedback_);
    return AppendKvInt(out, cap, len, "playing", state_.playing);
}
bool UINotesPage::consoleCommand(const char* args, char* reply, size_t cap) {
    if (!args)
        return false;
    char verb[16], extra;
    int a, b, c, d;
    if (std::sscanf(args, "%15s %d %d %d %d %c", verb, &a, &b, &c, &d, &extra) == 5 &&
        !std::strcmp(verb, "NOTE") && a >= 0 && a < 4 && b >= 0 && b <= 127 && c >= 0 && c <= 127 &&
        d >= 0 && d <= 32767) {
        lane_ = static_cast<uint8_t>(a);
        if (!lane({static_cast<uint8_t>(b), static_cast<uint8_t>(c), static_cast<uint16_t>(d)}))
            return false;
    } else if (std::sscanf(args, "%15s %d %c", verb, &a, &extra) == 2) {
        if (!std::strcmp(verb, "STEP") && a >= 0 && a < 64)
            selectStep(a - step_);
        else if (!std::strcmp(verb, "LANE") && a >= 0 && a < 4) {
            lane_ = static_cast<uint8_t>(a);
            render();
        } else if (!std::strcmp(verb, "MODE") && a >= 0 && a < 4)
            transport(SEQ_TRANSPORT_CONFIGURE, static_cast<uint8_t>(a), state_.quantize);
        else if (!std::strcmp(verb, "QUANTIZE") && a >= 0 && a < 3)
            transport(SEQ_TRANSPORT_CONFIGURE, state_.input_mode, static_cast<uint8_t>(a));
        else if (!std::strcmp(verb, "MELODIC") && a >= 0 && a < 2) {
            if (!edit(
                    {SEQ_OP_SET_MELODIC, getCurrentTrack(), step_, static_cast<uint8_t>(a), 0, 0}))
                return false;
        } else
            return false;
    } else
        return false;
    std::snprintf(reply, cap, "ok");
    return true;
}
void UINotesPage::tick(lv_timer_t* timer) {
    static_cast<UINotesPage*>(lv_timer_get_user_data(timer))->service();
}
std::shared_ptr<UIPage> createNotesPage(uint8_t step) {
    return std::make_shared<UINotesPage>(step);
}
}  // namespace wavex_ui
