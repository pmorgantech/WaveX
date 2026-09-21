#include "ui/ui_song_page.h"

#include <esp_random.h>

#include "../styles/ui_theme.h"
#include "debug/console_command.h"
#include "inter_mcu.h"
#include "ui/ui_navigator.h"
#include "ui/ui_pattern_slots_page.h"
#include "ui/ui_project_files_page.h"

#include "wxcf/pattern_file.hpp"
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
void text(lv_obj_t* object, const char* value) {
    if (std::strcmp(lv_label_get_text(object), value))
        lv_label_set_text(object, value);
}
const char* errorText(uint8_t error) {
    switch (error) {
        case SEQ_SONG_OK:
            return "Done. Project Save copy saves all Songs and Patterns.";
        case SEQ_SONG_BUSY:
            return "Another operation or Song playback is active. Stop and try again.";
        case SEQ_SONG_STOP_FIRST:
            return "Stop the sequencer, including MIDI-armed playback, before arranging or "
                   "starting a Song.";
        case SEQ_SONG_BAD_NAME:
            return "Use 1-23 letters/numbers, spaces, hyphens or underscores; no outer spaces.";
        case SEQ_SONG_EMPTY:
            return "Create this Song first.";
        case SEQ_SONG_EXISTS:
            return "This Song slot is occupied.";
        case SEQ_SONG_BAD_ENTRY:
            return "Choose an occupied Pattern. Keep 1-128 sections and at least one repeat.";
        case SEQ_SONG_NO_MEMORY:
            return "Not enough sample memory for a Project document.";
        default:
            return "Pattern capture did not finish. Stop editing and try again.";
    }
}
}  // namespace
void UISongPage::onEnter(lv_obj_t* parent) {
    root_ = lv_obj_create(parent);
    ui_theme_apply_container_style(root_, false);
    lv_obj_set_size(root_, UI_CONTENT_WIDTH, UI_CONTENT_HEIGHT);
    lv_obj_set_style_pad_all(root_, 0, 0);
    lv_obj_remove_flag(root_, LV_OBJ_FLAG_SCROLLABLE);
    auto button = [&](Control& c, int x, int y, int w, int h, uint8_t index) {
        c.owner = this;
        c.index = index;
        c.button = lv_button_create(root_);
        ui_theme_apply_button_style(c.button, false);
        lv_obj_set_style_bg_color(c.button, UI_COLOR_CARD_ALT, LV_STATE_DISABLED);
        lv_obj_set_style_border_color(c.button, UI_COLOR_LINE, LV_STATE_DISABLED);
        lv_obj_set_style_recolor_opa(c.button, LV_OPA_TRANSP, LV_STATE_DISABLED);
        lv_obj_set_style_opa(c.button, LV_OPA_70, LV_STATE_DISABLED);
        lv_obj_set_pos(c.button, x, y);
        lv_obj_set_size(c.button, w, h);
        c.label = lv_label_create(c.button);
        ui_theme_apply_label_style(c.label, false);
        lv_obj_set_style_text_font(c.label, UI_FONT_BODY, 0);
        lv_obj_center(c.label);
        lv_obj_add_event_cb(c.button, click, LV_EVENT_CLICKED, &c);
    };
    button(arrows_[0], UI_MARGIN_X, 0, UI_SLOT_BUTTON_W, UI_SLOT_BUTTON_H, 20);
    button(arrows_[1],
           UI_CONTENT_WIDTH - UI_MARGIN_X - UI_SLOT_BUTTON_W,
           0,
           UI_SLOT_BUTTON_W,
           UI_SLOT_BUTTON_H,
           21);
    text(arrows_[0].label, "Song -");
    text(arrows_[1].label, "Song +");
    heading_ = lv_label_create(root_);
    ui_theme_apply_label_style(heading_, false);
    lv_obj_set_pos(heading_, UI_SLOT_HEADING_X, UI_PADDING_SMALL);
    lv_obj_set_width(heading_, UI_CONTENT_WIDTH - 2 * UI_SLOT_HEADING_X);
    input_ = lv_textarea_create(root_);
    lv_textarea_set_one_line(input_, true);
    lv_textarea_set_max_length(input_, 23);
    lv_obj_set_style_anim_duration(input_, 0, LV_PART_CURSOR);
    lv_obj_set_style_anim_duration(input_, 0, LV_PART_CURSOR | LV_STATE_FOCUSED);
    lv_textarea_set_accepted_chars(
        input_, "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789 -_");
    lv_textarea_set_placeholder_text(input_, "Song name for Create / Shift + Rename");
    lv_obj_set_pos(input_, UI_MARGIN_X, UI_PROJECT_FILE_INPUT_Y);
    lv_obj_set_size(input_, UI_CONTENT_WIDTH - 2 * UI_MARGIN_X, UI_PROJECT_FILE_INPUT_H);
    lv_obj_set_style_bg_color(input_, UI_COLOR_CARD, 0);
    lv_obj_set_style_text_color(input_, UI_COLOR_FG, 0);
    lv_obj_set_style_text_font(input_, UI_FONT_BODY, 0);
    lv_obj_set_style_border_color(input_, UI_COLOR_LINE, 0);
    lv_obj_add_event_cb(
        input_,
        [](lv_event_t* e) {
            auto& self = *static_cast<UISongPage*>(lv_event_get_user_data(e));
            if (self.ready())
                lv_obj_remove_flag(self.keyboard_, LV_OBJ_FLAG_HIDDEN);
        },
        LV_EVENT_CLICKED,
        this);
    for (uint8_t i = 0; i < 6; ++i)
        button(rows_[i],
               UI_SONG_ROW_X,
               UI_SONG_ROW_Y + i * (UI_SONG_ROW_H + UI_SONG_ROW_GAP),
               UI_SONG_ROW_W,
               UI_SONG_ROW_H,
               i);
    for (uint8_t i = 0; i < 4; ++i)
        button(fields_[i],
               UI_SONG_FIELD_X,
               UI_SONG_ROW_Y + i * (UI_SONG_FIELD_H + UI_SONG_FIELD_GAP),
               UI_SONG_FIELD_W,
               UI_SONG_FIELD_H,
               10 + i);
    for (auto& field: fields_) {
        lv_obj_set_style_bg_color(field.button, UI_COLOR_CARD, 0);
        lv_obj_set_style_text_color(field.button, UI_COLOR_FG, 0);
    }
    hint_ = lv_label_create(root_);
    ui_theme_apply_label_style(hint_, false);
    lv_obj_set_style_text_font(hint_, UI_FONT_SMALL, 0);
    lv_obj_set_pos(hint_, UI_MARGIN_X, UI_SONG_HINT_Y);
    lv_obj_set_width(hint_, UI_CONTENT_WIDTH - 2 * UI_MARGIN_X);
    lv_label_set_long_mode(hint_, LV_LABEL_LONG_WRAP);
    keyboard_ = lv_keyboard_create(root_);
    lv_obj_set_size(keyboard_, UI_CONTENT_WIDTH - 2 * UI_PADDING_SMALL, UI_PROJECT_FILE_KEYBOARD_H);
    lv_obj_align(keyboard_, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_keyboard_set_textarea(keyboard_, input_);
    lv_obj_set_style_bg_color(keyboard_, UI_COLOR_BG, 0);
    lv_obj_set_style_bg_color(keyboard_, UI_COLOR_CARD, LV_PART_ITEMS);
    lv_obj_set_style_text_color(keyboard_, UI_COLOR_FG, LV_PART_ITEMS);
    lv_obj_set_style_text_font(keyboard_, UI_FONT_BODY, LV_PART_ITEMS);
    lv_obj_add_flag(keyboard_, LV_OBJ_FLAG_HIDDEN);
    auto close = [](lv_event_t* e) {
        auto& self = *static_cast<UISongPage*>(lv_event_get_user_data(e));
        lv_obj_add_flag(self.keyboard_, LV_OBJ_FLAG_HIDDEN);
    };
    lv_obj_add_event_cb(keyboard_, close, LV_EVENT_READY, this);
    lv_obj_add_event_cb(keyboard_, close, LV_EVENT_CANCEL, this);
    valid_ = false;
    dirty_ = false;
    pending_ = 0;
    soft_state_ = UINT32_MAX;
    alive_ = inter_mcu_backend_link_alive();
    std::snprintf(message_,
                  sizeof(message_),
                  "Select a field and turn the encoder. Apply saves the edit; Back reverts it. "
                  "Shift reveals arrangement actions.");
    read();
    timer_ = lv_timer_create(
        [](lv_timer_t* timer) {
            static_cast<UISongPage*>(lv_timer_get_user_data(timer))->service();
        },
        100,
        this);
    render();
}
void UISongPage::onExit() {
    if (timer_)
        lv_timer_delete(timer_);
    timer_ = nullptr;
    if (root_)
        lv_obj_delete(root_);
    root_ = nullptr;
    heading_ = input_ = keyboard_ = hint_ = nullptr;
    valid_ = false;
    pending_ = 0;
}
bool UISongPage::confirmed() const {
    return alive_ && valid_ && static_cast<uint32_t>(lv_tick_get() - received_at_) < 1500;
}
bool UISongPage::ready() const {
    return confirmed() && !status_.busy && !pending_;
}
void UISongPage::read() {
    if (!alive_)
        return;
    SeqSongOpMessage request;
    request.request_id = read_id_ = nextId();
    request.song = song_;
    sent_at_ = lv_tick_get();
    inter_mcu_send_seq_song_op(request);
}
void UISongPage::resetDraft() {
    section_ = std::min<uint8_t>(section_, status_.length ? status_.length - 1 : 0);
    pattern_ = status_.used ? status_.entries[section_].pattern : status_.active_pattern;
    repeats_ = status_.used ? status_.entries[section_].repeats : 1;
    tempo_ = status_.tempo_bpm_x100;
    dirty_ = false;
}
void UISongPage::service() {
    const bool alive = inter_mcu_backend_link_alive();
    if (alive != alive_) {
        alive_ = alive;
        valid_ = false;
        dirty_ = false;
        if (alive_)
            read();
    }
    SeqSongStatusMessage received;
    if (alive_ && inter_mcu_get_seq_song_status(&received) && received.request_id == read_id_ &&
        received.request_id != seen_ && received.song == song_ && IsValidSeqSongStatus(received)) {
        seen_ = received.request_id;
        status_ = received;
        valid_ = true;
        received_at_ = lv_tick_get();
        if (pending_ && status_.completed_request_id == pending_) {
            pending_ = 0;
            dirty_ = false;
            std::snprintf(message_, sizeof(message_), "%s", errorText(status_.error));
        }
        if (!dirty_)
            resetDraft();
    }
    if (pending_ && confirmed() && !status_.busy &&
        static_cast<uint32_t>(lv_tick_get() - pending_at_) > 10000) {
        pending_ = 0;
        dirty_ = false;
        std::snprintf(message_,
                      sizeof(message_),
                      "Operation unconfirmed. Inspect this Song before retrying.");
    }
    if (alive_ && static_cast<uint32_t>(lv_tick_get() - sent_at_) >= 300)
        read();
    render();
}
void UISongPage::song(int delta) {
    if (dirty_ || pending_)
        return;
    const auto next = static_cast<uint8_t>(std::clamp(static_cast<int>(song_) + delta, 0, 15));
    if (next == song_)
        return;
    song_ = next;
    section_ = 0;
    valid_ = false;
    read();
    render();
}
void UISongPage::section(int delta) {
    if (!confirmed() || dirty_ || pending_)
        return;
    section_ = static_cast<uint8_t>(
        std::clamp(static_cast<int>(section_) + delta, 0, std::max(0, status_.length - 1)));
    resetDraft();
    render();
}
void UISongPage::adjust(int delta) {
    if (!focus_) {
        section(delta);
        return;
    }
    if (!ready() || !status_.used)
        return;
    if (focus_ == 1)
        pattern_ = static_cast<uint8_t>(std::clamp(static_cast<int>(pattern_) + delta, 0, 127));
    else if (focus_ == 2)
        repeats_ = static_cast<uint8_t>(std::clamp(static_cast<int>(repeats_) + delta, 1, 255));
    else
        tempo_ =
            static_cast<uint16_t>(std::clamp(static_cast<int>(tempo_) + delta * 100, 2000, 30000));
    dirty_ = true;
    render();
}
void UISongPage::send(uint8_t op, int destination) {
    if (op == SEQ_SONG_STOP ? !confirmed() || pending_ : !ready())
        return;
    SeqSongOpMessage request;
    request.request_id = nextId();
    request.op = op;
    request.song = song_;
    request.entry = section_;
    request.pattern = pattern_;
    request.repeats = repeats_;
    request.tempo_bpm_x100 = tempo_;
    request.loop = loop_;
    if (destination >= 0)
        request.destination = static_cast<uint8_t>(destination);
    if (op == SEQ_SONG_INSERT)
        request.entry = static_cast<uint8_t>(section_ + 1);
    detail::CopyWireString(request.name, sizeof(request.name), lv_textarea_get_text(input_));
    if (op == SEQ_SONG_CREATE && !request.name[0])
        std::snprintf(request.name, sizeof(request.name), "Song %u", song_ + 1);
    if ((op == SEQ_SONG_CREATE || op == SEQ_SONG_RENAME) &&
        !WaveX::PatternFile::ValidName(request.name)) {
        std::snprintf(message_, sizeof(message_), "%s", errorText(SEQ_SONG_BAD_NAME));
        render();
        return;
    }
    if (inter_mcu_send_seq_song_op(request) == ESP_OK) {
        lv_obj_add_flag(keyboard_, LV_OBJ_FLAG_HIDDEN);
        pending_ = read_id_ = request.request_id;
        pending_at_ = sent_at_ = lv_tick_get();
        std::snprintf(message_, sizeof(message_), "Applying Song operation...");
    } else
        std::snprintf(message_, sizeof(message_), "Link busy. Try again.");
    render();
}
void UISongPage::click(lv_event_t* event) {
    auto& c = *static_cast<Control*>(lv_event_get_user_data(event));
    auto& self = *c.owner;
    if (c.index >= 20)
        self.song(c.index == 20 ? -1 : 1);
    else if (c.index >= 10) {
        const uint8_t next = c.index - 10;
        if (!self.dirty_ || (next > 0 && next < 3 && self.focus_ < 3)) {
            self.focus_ = next;
            self.render();
        }
    } else
        self.section(static_cast<int>((self.section_ / 6) * 6 + c.index) - self.section_);
}
void UISongPage::render() {
    char value[160];
    std::snprintf(value,
                  sizeof(value),
                  "Song %u: %s",
                  song_ + 1,
                  valid_ ? (status_.used ? status_.name : "Empty") : "Reading...");
    text(heading_, value);
    for (uint8_t i = 0; i < 6; ++i) {
        const unsigned entry = (section_ / 6) * 6 + i;
        const bool exists = valid_ && entry < status_.length;
        const bool playing =
            confirmed() && status_.playing_song == song_ && status_.playing_entry == entry;
        if (exists)
            std::snprintf(value,
                          sizeof(value),
                          "%s %3u     Pattern %3u     x %u",
                          playing ? ">" : " ",
                          entry + 1,
                          status_.entries[entry].pattern + 1,
                          status_.entries[entry].repeats);
        else
            std::snprintf(value, sizeof(value), "--");
        text(rows_[i].label, value);
        const auto border = exists && entry == section_ ? UI_COLOR_ACCENT : UI_COLOR_LINE;
        if (!lv_color_eq(lv_obj_get_style_border_color(rows_[i].button, LV_PART_MAIN), border))
            lv_obj_set_style_border_color(rows_[i].button, border, 0);
        const auto fill = playing ? UI_COLOR_CARD_ALT : UI_COLOR_CARD;
        if (!lv_color_eq(lv_obj_get_style_bg_color(rows_[i].button, LV_PART_MAIN), fill))
            lv_obj_set_style_bg_color(rows_[i].button, fill, 0);
    }
    const char* titles[] = {"Section", "Pattern", "Repeats", "Tempo"};
    const unsigned values[] = {
        static_cast<unsigned>(section_ + 1), static_cast<unsigned>(pattern_ + 1), repeats_, tempo_};
    for (uint8_t i = 0; i < 4; ++i) {
        if (i == 3)
            std::snprintf(value, sizeof(value), "%s: %.2f BPM", titles[i], tempo_ / 100.0);
        else
            std::snprintf(value, sizeof(value), "%s: %u", titles[i], values[i]);
        text(fields_[i].label, value);
        const auto border = i == focus_ ? UI_COLOR_ACCENT : UI_COLOR_LINE;
        if (!lv_color_eq(lv_obj_get_style_border_color(fields_[i].button, LV_PART_MAIN), border))
            lv_obj_set_style_border_color(fields_[i].button, border, 0);
    }
    if (!alive_)
        text(hint_, "Audio engine disconnected");
    else if (confirmed() && status_.playing_song != 0xff) {
        std::snprintf(value,
                      sizeof(value),
                      "Playing Song %u / section %u / repeat %u. %s. Stop to edit. Voice tails "
                      "continue after Song end.",
                      status_.playing_song + 1,
                      status_.playing_entry + 1,
                      status_.playing_repeat,
                      status_.loop ? "Loop" : "Play once");
        text(hint_, value);
    } else
        text(hint_,
             dirty_ ? "Unsaved field edit: Apply confirms; Revert discards. Other sections are "
                      "locked until then."
                    : message_);
    auto disable = [](lv_obj_t* object, bool disabled) {
        if (lv_obj_has_state(object, LV_STATE_DISABLED) == disabled)
            return;
        if (disabled)
            lv_obj_add_state(object, LV_STATE_DISABLED);
        else
            lv_obj_remove_state(object, LV_STATE_DISABLED);
    };
    disable(input_, !ready());
    for (auto& arrow: arrows_)
        disable(arrow.button, dirty_ || pending_);
    for (uint8_t i = 1; i < 4; ++i)
        disable(fields_[i].button, !ready() || !status_.used);
    const uint32_t soft =
        (ready() ? 1u : 0) | (status_.used ? 2u : 0) | (dirty_ ? 4u : 0) | (loop_ ? 8u : 0) |
        (confirmed() && status_.playing_song != 0xff ? 16u : 0) | (pending_ ? 32u : 0) |
        (static_cast<uint32_t>(section_) << 8) | (static_cast<uint32_t>(status_.length) << 16);
    if (soft != soft_state_) {
        soft_state_ = soft;
        UINavigator::instance().refreshSoftkeys();
    }
}
std::array<Softkey, NUM_SOFTKEYS> UISongPage::getSoftkeys() {
    std::array<Softkey, NUM_SOFTKEYS> keys{};
    const bool editable = ready() && status_.used;
    const bool playing = confirmed() && status_.playing_song != 0xff;
    keys[0] = {dirty_ ? "Revert" : "Back", [this] {
                   if (dirty_) {
                       resetDraft();
                       render();
                   } else
                       UINavigator::instance().pop();
               }};
    keys[1] = {status_.used ? "Apply" : "Create",
               [this] {
                   send(!status_.used ? SEQ_SONG_CREATE
                        : focus_ == 3 ? SEQ_SONG_TEMPO
                                      : SEQ_SONG_SET_ENTRY);
               },
               ready() && (!status_.used || dirty_),
               "Edit a field first; stop playback to arrange"};
    keys[2] = {"Insert after",
               [this] { send(SEQ_SONG_INSERT); },
               editable && !dirty_ && status_.length < 128,
               "Stop playback; maximum 128 sections"};
    keys[3] = {"Remove",
               [this] { send(SEQ_SONG_REMOVE); },
               editable && !dirty_ && status_.length > 1,
               "Keep at least one section"};
    keys[4] = {playing ? "Stop" : "Play here",
               [this, playing] { send(playing ? SEQ_SONG_STOP : SEQ_SONG_PLAY); },
               !pending_ && (playing || (editable && !dirty_)),
               "Read a Song and apply edits first"};
    keys[5] = {loop_ ? "Loop: on" : "Loop: off",
               [this] {
                   loop_ = !loop_;
                   render();
               },
               ready() && !dirty_,
               "Stop playback first"};
    return keys;
}
std::array<Softkey, NUM_SOFTKEYS> UISongPage::getShiftedSoftkeys() {
    auto keys = getSoftkeys();
    for (unsigned i = 1; i < keys.size(); ++i)
        keys[i] = {};
    const bool editable = ready() && status_.used && !dirty_;
    keys[1] = {"Rename",
               [this] { send(SEQ_SONG_RENAME); },
               editable,
               "Stop playback and apply edits first"};
    keys[2] = {"Move up",
               [this] { send(SEQ_SONG_MOVE, section_ - 1); },
               editable && section_ > 0,
               "Select a later section"};
    keys[3] = {"Move down",
               [this] { send(SEQ_SONG_MOVE, section_ + 1); },
               editable && section_ + 1 < status_.length,
               "Select an earlier section"};
    keys[4] = {"Patterns",
               [] { UINavigator::instance().push(createPatternSlotsPage()); },
               !dirty_ && !pending_,
               "Apply or revert first"};
    keys[5] = {"Project",
               [] { UINavigator::instance().push(createProjectFilesPage()); },
               !dirty_ && !pending_,
               "Apply or revert first"};
    return keys;
}
void UISongPage::onInput(const InputEvent& e) {
    if (e.type == InputType::EncoderLeft || e.type == InputType::EncoderRight)
        adjust(e.steps());
    else
        UIPage::onInput(e);
}
size_t UISongPage::consoleState(char* out, size_t cap, size_t len) {
    using namespace WaveX::Debug;
    len = AppendKvInt(out, cap, len, "song", song_ + 1);
    len = AppendKvInt(out, cap, len, "songready", ready());
    len = AppendKvInt(out, cap, len, "section", section_ + 1);
    len = AppendKvInt(out, cap, len, "songlength", status_.length);
    return AppendKvInt(out, cap, len, "songplaying", confirmed() && status_.playing_song != 0xff);
}
bool UISongPage::consoleCommand(const char* args, char* reply, size_t cap) {
    if (!args || !root_)
        return false;
    int value = 0;
    char extra;
    if (!std::strncmp(args, "NAME ", 5) && ready())
        lv_textarea_set_text(input_, args + 5);
    else if (std::sscanf(args, "SONG %d %c", &value, &extra) == 1 && value >= 1 && value <= 16)
        song(value - 1 - song_);
    else if (std::sscanf(args, "SECTION %d %c", &value, &extra) == 1 && value >= 1 && value <= 128)
        section(value - 1 - section_);
    else if (std::sscanf(args, "FIELD %d %c", &value, &extra) == 1 && value >= 0 && value <= 3 &&
             !dirty_) {
        focus_ = static_cast<uint8_t>(value);
        render();
    } else
        return false;
    std::snprintf(reply, cap, "ok");
    return true;
}
std::shared_ptr<UIPage> createSongPage() {
    return std::make_shared<UISongPage>();
}
}  // namespace wavex_ui
