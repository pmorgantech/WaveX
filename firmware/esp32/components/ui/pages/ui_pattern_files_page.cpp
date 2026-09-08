#include "ui/ui_pattern_files_page.h"

#include <esp_random.h>

#include "../styles/ui_theme.h"
#include "debug/console_command.h"
#include "inter_mcu.h"
#include "ui/ui_navigator.h"

#include "wxcf/pattern_file.hpp"
#include <cstdio>
#include <cstring>

namespace wavex_ui {
using namespace WaveX::Protocol;
namespace {
uint32_t nextId() {
    static uint32_t id = 0;
    if (!id)
        id = esp_random();
    if (++id == 0)
        ++id;
    return id;
}
void label(lv_obj_t* object, const char* text) {
    if (object && std::strcmp(lv_label_get_text(object), text))
        lv_label_set_text(object, text);
}
const char* errorText(uint8_t error) {
    switch (error) {
        case SEQ_FILE_OK:
            return "Done. Your Track instruments and tempo are unchanged.";
        case SEQ_FILE_BUSY:
            return "Another pattern operation is busy. Try again when it finishes.";
        case SEQ_FILE_BAD_NAME:
            return "Use 1-23 letters/numbers, spaces, hyphens or underscores; no outer spaces.";
        case SEQ_FILE_NOT_FOUND:
            return "Pattern not found. Enter the saved name without its extension.";
        case SEQ_FILE_EXISTS:
            return "That name already exists. Choose a new name for this copy.";
        case SEQ_FILE_BAD_FILE:
            return "Invalid or unsupported pattern file. The working pattern is unchanged.";
        case SEQ_FILE_CAPTURE_BUSY:
            return "Pattern kept changing during capture. Pause editing and save again.";
        default:
            return "Card operation failed. Check the card and retry.";
    }
}
}  // namespace
void UIPatternFilesPage::onEnter(lv_obj_t* parent) {
    root_ = lv_obj_create(parent);
    ui_theme_apply_container_style(root_, false);
    lv_obj_set_size(root_, UI_CONTENT_WIDTH, UI_CONTENT_HEIGHT);
    lv_obj_set_style_pad_all(root_, 0, 0);
    lv_obj_remove_flag(root_, LV_OBJ_FLAG_SCROLLABLE);
    last_ = lv_label_create(root_);
    ui_theme_apply_label_style(last_, false);
    lv_obj_set_pos(last_, 20, 10);
    lv_obj_set_width(last_, 1220);
    input_ = lv_textarea_create(root_);
    lv_textarea_set_one_line(input_, true);
    lv_textarea_set_max_length(input_, SEQ_FILE_NAME_BYTES - 1);
    lv_textarea_set_accepted_chars(
        input_, "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789 -_");
    lv_obj_set_pos(input_, 20, 55);
    lv_obj_set_size(input_, 1220, 65);
    lv_textarea_set_placeholder_text(input_, "Pattern name");
    lv_obj_set_style_bg_color(input_, UI_COLOR_CARD, 0);
    lv_obj_set_style_text_color(input_, UI_COLOR_FG, 0);
    lv_obj_set_style_text_font(input_, UI_FONT_BODY, 0);
    lv_obj_set_style_border_color(input_, UI_COLOR_LINE, 0);
    hint_ = lv_label_create(root_);
    ui_theme_apply_label_style(hint_, false);
    lv_obj_set_style_text_font(hint_, UI_FONT_SMALL, 0);
    lv_obj_set_pos(hint_, 20, 132);
    lv_obj_set_width(hint_, 1220);
    lv_label_set_long_mode(hint_, LV_LABEL_LONG_WRAP);
    keyboard_ = lv_keyboard_create(root_);
    lv_obj_set_size(keyboard_, 1240, 340);
    lv_obj_set_style_bg_color(keyboard_, UI_COLOR_BG, 0);
    lv_obj_set_style_bg_color(keyboard_, UI_COLOR_CARD, LV_PART_ITEMS);
    lv_obj_set_style_text_color(keyboard_, UI_COLOR_FG, LV_PART_ITEMS);
    lv_obj_set_style_text_font(keyboard_, UI_FONT_BODY, LV_PART_ITEMS);
    lv_obj_set_style_border_color(keyboard_, UI_COLOR_LINE, LV_PART_ITEMS);
    lv_obj_set_style_border_width(keyboard_, 1, LV_PART_ITEMS);
    lv_obj_set_style_bg_color(keyboard_, UI_COLOR_CARD_ALT, LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_set_style_text_color(keyboard_, UI_COLOR_FG, LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_set_style_bg_color(keyboard_, UI_COLOR_ACCENT, LV_PART_ITEMS | LV_STATE_PRESSED);
    lv_obj_set_style_text_color(keyboard_, UI_COLOR_ACCENT_FG, LV_PART_ITEMS | LV_STATE_PRESSED);
    lv_obj_align(keyboard_, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_keyboard_set_textarea(keyboard_, input_);
    lv_obj_add_event_cb(keyboard_, keyboardEvent, LV_EVENT_READY, this);
    lv_obj_add_event_cb(keyboard_, keyboardEvent, LV_EVENT_CANCEL, this);
    lv_obj_add_event_cb(input_, inputEvent, LV_EVENT_CLICKED, this);
    std::snprintf(message_,
                  sizeof(message_),
                  "Save/load in wavex/patterns. File operations stop the streaming preview; Track "
                  "voices continue.");
    alive_ = inter_mcu_backend_link_alive();
    read();
    timer_ = lv_timer_create(tick, 100, this);
    render();
}
void UIPatternFilesPage::onExit() {
    if (timer_)
        lv_timer_delete(timer_);
    timer_ = nullptr;
    if (root_)
        lv_obj_delete(root_);
    root_ = input_ = keyboard_ = hint_ = last_ = nullptr;
    pending_id_ = 0;
    valid_ = false;
    confirm_ = 0;
}
bool UIPatternFilesPage::ready() const {
    return alive_ && valid_ && !status_.busy && !pending_id_;
}
void UIPatternFilesPage::read() {
    if (!alive_)
        return;
    SeqFileOpMessage request;
    request.request_id = read_id_ = nextId();
    requested_at_ = lv_tick_get();
    inter_mcu_send_seq_file_op(request);
}
void UIPatternFilesPage::service() {
    const bool alive = inter_mcu_backend_link_alive();
    if (alive != alive_) {
        alive_ = alive;
        valid_ = false;
        pending_id_ = 0;
        confirm_ = 0;
        std::snprintf(message_,
                      sizeof(message_),
                      "%s",
                      alive
                          ? "Reading file status. Check the last file if an earlier operation was "
                            "unconfirmed."
                          : "Audio engine disconnected. Pending operation outcome is unconfirmed.");
        if (alive)
            read();
        UINavigator::instance().refreshSoftkeys();
    }
    SeqFileStatusMessage received;
    if (alive_ && inter_mcu_get_seq_file_status(&received) && received.request_id == read_id_ &&
        received.busy <= 1 && received.error <= SEQ_FILE_CAPTURE_BUSY &&
        received.completed_op <= SEQ_FILE_NEW &&
        std::memchr(received.name, 0, sizeof(received.name))) {
        const bool changed = !valid_ || std::memcmp(&status_, &received, sizeof(status_));
        status_ = received;
        valid_ = true;
        if (pending_id_ && received.completed_request_id == pending_id_) {
            pending_id_ = 0;
            std::snprintf(message_, sizeof(message_), "%s", errorText(received.error));
        }
        if (changed)
            UINavigator::instance().refreshSoftkeys();
    }
    if (pending_id_ && static_cast<uint32_t>(lv_tick_get() - pending_at_) > 10000) {
        pending_id_ = 0;
        std::snprintf(message_,
                      sizeof(message_),
                      "Operation not confirmed. Check the last file before retrying.");
        UINavigator::instance().refreshSoftkeys();
    }
    if (alive_ && static_cast<uint32_t>(lv_tick_get() - requested_at_) >= 300)
        read();
    render();
}
void UIPatternFilesPage::render() {
    char last[96];
    std::snprintf(last,
                  sizeof(last),
                  "Last saved/loaded: %s",
                  valid_ && status_.name[0] ? status_.name : "none");
    label(last_, last);
    label(hint_, !alive_ ? "Audio engine disconnected" : message_);
    if (confirm_ || pending_id_ || (valid_ && status_.busy)) {
        lv_obj_add_state(input_, LV_STATE_DISABLED);
        lv_obj_add_state(keyboard_, LV_STATE_DISABLED);
    } else {
        lv_obj_remove_state(input_, LV_STATE_DISABLED);
        lv_obj_remove_state(keyboard_, LV_STATE_DISABLED);
    }
}
void UIPatternFilesPage::choose(uint8_t op) {
    if (!ready())
        return;
    if (op != SEQ_FILE_NEW) {
        const auto* name = lv_textarea_get_text(input_);
        if (!WaveX::PatternFile::ValidName(name)) {
            std::snprintf(message_, sizeof(message_), "%s", errorText(SEQ_FILE_BAD_NAME));
            render();
            return;
        }
        std::snprintf(name_, sizeof(name_), "%s", name);
    } else
        name_[0] = 0;
    if (op == SEQ_FILE_SAVE_COPY) {
        send(op);
        return;
    }
    confirm_ = op;
    std::snprintf(message_,
                  sizeof(message_),
                  "%s",
                  op == SEQ_FILE_LOAD ? "Replace the working pattern with this file? Loading stops "
                                        "the sequencer. Unsaved edits will be lost."
                                      : "Clear all pattern steps and reset groove settings? This "
                                        "stops the sequencer. Unsaved edits will be lost.");
    render();
    UINavigator::instance().refreshSoftkeys();
}
void UIPatternFilesPage::send(uint8_t op) {
    if (!ready())
        return;
    SeqFileOpMessage request;
    request.request_id = nextId();
    request.op = op;
    detail::CopyWireString(request.name, sizeof(request.name), name_);
    confirm_ = 0;
    if (inter_mcu_send_seq_file_op(request) != ESP_OK) {
        std::snprintf(message_, sizeof(message_), "Link busy. Try again.");
    } else {
        pending_id_ = read_id_ = request.request_id;
        pending_at_ = requested_at_ = lv_tick_get();
        std::snprintf(message_,
                      sizeof(message_),
                      "%s",
                      op == SEQ_FILE_SAVE_COPY ? "Saving copy..." : "Applying pattern...");
    }
    render();
    UINavigator::instance().refreshSoftkeys();
}
void UIPatternFilesPage::cancel() {
    confirm_ = 0;
    std::snprintf(message_, sizeof(message_), "Cancelled. The working pattern is unchanged.");
    render();
    UINavigator::instance().refreshSoftkeys();
}
void UIPatternFilesPage::tick(lv_timer_t* timer) {
    static_cast<UIPatternFilesPage*>(lv_timer_get_user_data(timer))->service();
}
void UIPatternFilesPage::keyboardEvent(lv_event_t* event) {
    auto* self = static_cast<UIPatternFilesPage*>(lv_event_get_user_data(event));
    lv_obj_add_flag(self->keyboard_, LV_OBJ_FLAG_HIDDEN);
}
void UIPatternFilesPage::inputEvent(lv_event_t* event) {
    auto* self = static_cast<UIPatternFilesPage*>(lv_event_get_user_data(event));
    if (self->ready() && !self->confirm_)
        lv_obj_remove_flag(self->keyboard_, LV_OBJ_FLAG_HIDDEN);
}
void UIPatternFilesPage::onInput(const InputEvent& event) {
    UIPage::onInput(event);
}
std::array<Softkey, NUM_SOFTKEYS> UIPatternFilesPage::getSoftkeys() {
    std::array<Softkey, NUM_SOFTKEYS> keys{};
    if (confirm_) {
        keys[0] = {"Cancel", [this] { cancel(); }};
        keys[1] = {"Confirm", [this] { send(confirm_); }, ready(), "Waiting for the audio engine"};
    } else {
        keys[0] = {"Back", [] { UINavigator::instance().pop(); }};
        keys[1] = {"Save copy",
                   [this] { choose(SEQ_FILE_SAVE_COPY); },
                   ready(),
                   "Waiting for the audio engine"};
        keys[2] = {
            "Load", [this] { choose(SEQ_FILE_LOAD); }, ready(), "Waiting for the audio engine"};
        keys[3] = {
            "New", [this] { choose(SEQ_FILE_NEW); }, ready(), "Waiting for the audio engine"};
    }
    return keys;
}
size_t UIPatternFilesPage::consoleState(char* out, size_t cap, size_t len) {
    using namespace WaveX::Debug;
    len = AppendKvInt(out, cap, len, "fileready", ready());
    len = AppendKvInt(out, cap, len, "filebusy", status_.busy);
    len = AppendKvInt(out, cap, len, "filepending", pending_id_ != 0);
    len = AppendKvInt(out, cap, len, "fileerror", status_.error);
    len = AppendKvInt(out, cap, len, "fileconfirm", confirm_);
    return AppendKvText(out, cap, len, "filename", valid_ ? status_.name : "");
}
bool UIPatternFilesPage::consoleCommand(const char* args, char* reply, size_t cap) {
    if (!args || std::strncmp(args, "NAME ", 5) || !input_ || !ready() || confirm_)
        return false;
    lv_textarea_set_text(input_, args + 5);
    std::snprintf(reply, cap, "ok");
    return true;
}
std::shared_ptr<UIPage> createPatternFilesPage() {
    return std::make_shared<UIPatternFilesPage>();
}
}  // namespace wavex_ui
