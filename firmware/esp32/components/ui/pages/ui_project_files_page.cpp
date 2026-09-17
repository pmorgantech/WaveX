#include "ui/ui_project_files_page.h"

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
        case PROJECT_OK:
            return "Done. Project state is saved or restored. Load / New leaves playback stopped.";
        case PROJECT_BUSY:
            return "Another storage operation is busy. Try again when it finishes.";
        case PROJECT_BAD_NAME:
            return "Use 1-23 letters/numbers, spaces, hyphens or underscores; no outer spaces.";
        case PROJECT_NOT_FOUND:
            return "Project not found. Enter the saved name without its extension.";
        case PROJECT_EXISTS:
            return "That name already exists. Choose a new name for this copy.";
        case PROJECT_BAD_FILE:
            return "Invalid or unsupported Project. Your current session is retained.";
        case PROJECT_CAPTURE_BUSY:
            return "Capture could not finish. Pause editing and save again.";
        case PROJECT_NO_SPACE:
            return "Not enough free space on the card. Free space and try again.";
        case PROJECT_NO_MEMORY:
            return "Not enough sample memory to stage this Project. The current session is "
                   "retained.";
        case PROJECT_DEPENDENCY:
            return "An Instrument, WAV or Bank is missing, changed or unsupported. The current "
                   "session is retained.";
        case PROJECT_AUDIO_BUSY:
            return "Audio stop was not acknowledged. The current session is retained.";
        default:
            return "Card operation failed. Check the card; the previous Project file is preserved.";
    }
}
}  // namespace
void UIProjectFilesPage::onEnter(lv_obj_t* parent) {
    root_ = lv_obj_create(parent);
    ui_theme_apply_container_style(root_, false);
    lv_obj_set_size(root_, UI_CONTENT_WIDTH, UI_CONTENT_HEIGHT);
    lv_obj_set_style_pad_all(root_, 0, 0);
    lv_obj_remove_flag(root_, LV_OBJ_FLAG_SCROLLABLE);
    last_ = lv_label_create(root_);
    ui_theme_apply_label_style(last_, false);
    lv_obj_set_pos(last_, UI_MARGIN_X, UI_PADDING_SMALL);
    lv_obj_set_width(last_, UI_CONTENT_WIDTH - 2 * UI_MARGIN_X);
    input_ = lv_textarea_create(root_);
    lv_textarea_set_one_line(input_, true);
    lv_textarea_set_max_length(input_, PROJECT_NAME_BYTES - 1);
    lv_textarea_set_accepted_chars(
        input_, "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789 -_");
    lv_obj_set_pos(input_, UI_MARGIN_X, UI_PROJECT_FILE_INPUT_Y);
    lv_obj_set_size(input_, UI_CONTENT_WIDTH - 2 * UI_MARGIN_X, UI_PROJECT_FILE_INPUT_H);
    lv_textarea_set_placeholder_text(input_, "Project name");
    lv_obj_set_style_bg_color(input_, UI_COLOR_CARD, 0);
    lv_obj_set_style_text_color(input_, UI_COLOR_FG, 0);
    lv_obj_set_style_text_font(input_, UI_FONT_BODY, 0);
    lv_obj_set_style_border_color(input_, UI_COLOR_LINE, 0);
    hint_ = lv_label_create(root_);
    ui_theme_apply_label_style(hint_, false);
    lv_obj_set_style_text_font(hint_, UI_FONT_SMALL, 0);
    lv_obj_set_pos(hint_, UI_MARGIN_X, UI_PROJECT_FILE_HINT_Y);
    lv_obj_set_width(hint_, UI_CONTENT_WIDTH - 2 * UI_MARGIN_X);
    lv_label_set_long_mode(hint_, LV_LABEL_LONG_WRAP);
    keyboard_ = lv_keyboard_create(root_);
    lv_obj_set_size(keyboard_, UI_CONTENT_WIDTH - 2 * UI_PADDING_SMALL, UI_PROJECT_FILE_KEYBOARD_H);
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
    std::snprintf(
        message_,
        sizeof(message_),
        "Save a new Project copy with Instruments, sample edits, mixer, Patterns and Songs. "
        "WAV files stay at their current card paths. Edits are paused during file operations.");
    alive_ = inter_mcu_backend_link_alive();
    read();
    timer_ = lv_timer_create(tick, 100, this);
    render();
}
void UIProjectFilesPage::onExit() {
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
bool UIProjectFilesPage::ready() const {
    return alive_ && valid_ && static_cast<uint32_t>(lv_tick_get() - received_at_) < 1500 &&
           !status_.busy && !pending_id_;
}
void UIProjectFilesPage::read() {
    if (!alive_)
        return;
    ProjectOpMessage request;
    request.request_id = read_id_ = nextId();
    requested_at_ = lv_tick_get();
    inter_mcu_send_project_op(request);
}
void UIProjectFilesPage::service() {
    const bool alive = inter_mcu_backend_link_alive();
    if (alive != alive_) {
        alive_ = alive;
        valid_ = false;
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
    ProjectStatusMessage received;
    if (alive_ && inter_mcu_get_project_status(&received) && received.request_id == read_id_ &&
        received.request_id != seen_read_id_ && IsValidProjectStatus(received)) {
        seen_read_id_ = received.request_id;
        received_at_ = lv_tick_get();
        const bool changed = !valid_ || std::memcmp(&status_, &received, sizeof(status_));
        status_ = received;
        valid_ = true;
        if (pending_id_ && received.completed_request_id == pending_id_) {
            pending_id_ = 0;
            if (received.failed_track < 16 && received.error != PROJECT_OK)
                std::snprintf(message_,
                              sizeof(message_),
                              "Track %u: %s",
                              received.failed_track + 1,
                              errorText(received.error));
            else
                std::snprintf(message_, sizeof(message_), "%s", errorText(received.error));
        }
        if (changed)
            UINavigator::instance().refreshSoftkeys();
    }
    if (valid_ && static_cast<uint32_t>(lv_tick_get() - received_at_) >= 1500) {
        valid_ = false;
        UINavigator::instance().refreshSoftkeys();
    }
    if (pending_id_ && valid_ && !status_.busy &&
        static_cast<uint32_t>(lv_tick_get() - pending_at_) > 10000) {
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
void UIProjectFilesPage::render() {
    char last[96];
    std::snprintf(last,
                  sizeof(last),
                  "Last saved/loaded: %s",
                  valid_ && status_.name[0] ? status_.name : "none");
    label(last_, last);
    char progress[192];
    if (valid_ && status_.busy) {
        std::snprintf(progress,
                      sizeof(progress),
                      "%s Project... %u%%. Please wait.",
                      status_.active_op == PROJECT_SAVE_COPY ? "Saving" : "Loading",
                      status_.progress);
        label(hint_, progress);
    } else
        label(hint_, !alive_ ? "Audio engine disconnected" : message_);
    if (confirm_ || pending_id_ || (valid_ && status_.busy)) {
        lv_obj_add_state(input_, LV_STATE_DISABLED);
        lv_obj_add_state(keyboard_, LV_STATE_DISABLED);
    } else {
        lv_obj_remove_state(input_, LV_STATE_DISABLED);
        lv_obj_remove_state(keyboard_, LV_STATE_DISABLED);
    }
}
void UIProjectFilesPage::choose(uint8_t op) {
    if (!ready())
        return;
    if (op != PROJECT_NEW) {
        const auto* name = lv_textarea_get_text(input_);
        if (!WaveX::PatternFile::ValidName(name)) {
            std::snprintf(message_, sizeof(message_), "%s", errorText(PROJECT_BAD_NAME));
            render();
            return;
        }
        std::snprintf(name_, sizeof(name_), "%s", name);
    } else
        name_[0] = 0;
    if (op == PROJECT_SAVE_COPY) {
        send(op);
        return;
    }
    confirm_ = op;
    std::snprintf(
        message_,
        sizeof(message_),
        "%s",
        op == PROJECT_LOAD
            ? "Load this Project and replace the current session? Playback stops. "
              "Unsaved Project, Instrument and pattern edits will be lost."
            : "Start an empty Project? This stops playback and clears Track "
              "assignments, Patterns, Songs and mixer settings. Unsaved edits will be lost.");
    render();
    UINavigator::instance().refreshSoftkeys();
}
void UIProjectFilesPage::send(uint8_t op) {
    if (!ready())
        return;
    ProjectOpMessage request;
    request.request_id = nextId();
    request.op = op;
    detail::CopyWireString(request.name, sizeof(request.name), name_);
    confirm_ = 0;
    if (inter_mcu_send_project_op(request) != ESP_OK) {
        std::snprintf(message_, sizeof(message_), "Link busy. Try again.");
    } else {
        pending_id_ = read_id_ = request.request_id;
        pending_at_ = requested_at_ = lv_tick_get();
        std::snprintf(message_,
                      sizeof(message_),
                      "%s",
                      op == PROJECT_SAVE_COPY ? "Saving copy..." : "Applying Project...");
    }
    render();
    UINavigator::instance().refreshSoftkeys();
}
void UIProjectFilesPage::cancel() {
    confirm_ = 0;
    std::snprintf(message_, sizeof(message_), "Cancelled. The current Project is unchanged.");
    render();
    UINavigator::instance().refreshSoftkeys();
}
void UIProjectFilesPage::tick(lv_timer_t* timer) {
    static_cast<UIProjectFilesPage*>(lv_timer_get_user_data(timer))->service();
}
void UIProjectFilesPage::keyboardEvent(lv_event_t* event) {
    auto* self = static_cast<UIProjectFilesPage*>(lv_event_get_user_data(event));
    lv_obj_add_flag(self->keyboard_, LV_OBJ_FLAG_HIDDEN);
}
void UIProjectFilesPage::inputEvent(lv_event_t* event) {
    auto* self = static_cast<UIProjectFilesPage*>(lv_event_get_user_data(event));
    if (self->ready() && !self->confirm_)
        lv_obj_remove_flag(self->keyboard_, LV_OBJ_FLAG_HIDDEN);
}
void UIProjectFilesPage::onInput(const InputEvent& event) {
    UIPage::onInput(event);
}
std::array<Softkey, NUM_SOFTKEYS> UIProjectFilesPage::getSoftkeys() {
    std::array<Softkey, NUM_SOFTKEYS> keys{};
    if (confirm_) {
        keys[0] = {"Cancel", [this] { cancel(); }};
        keys[1] = {"Confirm", [this] { send(confirm_); }, ready(), "Waiting for the audio engine"};
    } else {
        keys[0] = {"Back", [] { UINavigator::instance().pop(); }};
        keys[1] = {"Save copy",
                   [this] { choose(PROJECT_SAVE_COPY); },
                   ready(),
                   "Waiting for the audio engine"};
        keys[2] = {
            "Load", [this] { choose(PROJECT_LOAD); }, ready(), "Waiting for the audio engine"};
        keys[3] = {"New", [this] { choose(PROJECT_NEW); }, ready(), "Waiting for the audio engine"};
    }
    return keys;
}
size_t UIProjectFilesPage::consoleState(char* out, size_t cap, size_t len) {
    using namespace WaveX::Debug;
    len = AppendKvInt(out, cap, len, "projectready", ready());
    len = AppendKvInt(out, cap, len, "projectbusy", status_.busy);
    len = AppendKvInt(out, cap, len, "projectpending", pending_id_ != 0);
    len = AppendKvInt(out, cap, len, "projecterror", status_.error);
    len = AppendKvInt(out, cap, len, "projectconfirm", confirm_);
    return AppendKvText(out, cap, len, "projectname", valid_ ? status_.name : "");
}
bool UIProjectFilesPage::consoleCommand(const char* args, char* reply, size_t cap) {
    if (!args || std::strncmp(args, "NAME ", 5) || !input_ || !ready() || confirm_)
        return false;
    lv_textarea_set_text(input_, args + 5);
    std::snprintf(reply, cap, "ok");
    return true;
}
std::shared_ptr<UIPage> createProjectFilesPage() {
    return std::make_shared<UIProjectFilesPage>();
}
}  // namespace wavex_ui
