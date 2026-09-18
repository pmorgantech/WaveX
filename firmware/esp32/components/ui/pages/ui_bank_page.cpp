#include "ui/ui_bank_page.h"

#include <esp_random.h>

#include "../styles/ui_theme.h"
#include "debug/console_command.h"
#include "inter_mcu.h"
#include "ui/current_track.h"
#include "ui/ui_navigator.h"

#include "wxcf/bank_file.hpp"
#include <cstdio>
#include <cstring>

namespace wavex_ui {
using namespace WaveX::Protocol;
namespace {
bool isTransfer(uint8_t op) {
    return op == BANK_COPY_SLOT || op == BANK_MOVE_SLOT;
}
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
        case BANK_OK:
            return "Done. Bank copies preserve earlier files; recalled sounds are private Track "
                   "copies.";
        case BANK_BUSY:
            return "Another storage operation is busy. Try again when it finishes.";
        case BANK_BAD_NAME:
            return "Use 1-23 letters/numbers, spaces, hyphens or underscores; no outer spaces.";
        case BANK_NOT_FOUND:
            return "Bank not found. Enter its saved name without .wxb.";
        case BANK_EXISTS:
            return "That name already exists. Choose a new name for the copy.";
        case BANK_BAD_FILE:
            return "Invalid Bank. The current Bank and Track are retained.";
        case BANK_NO_SPACE:
            return "Not enough free space on the card.";
        case BANK_NO_MEMORY:
            return "Not enough memory for Bank samples. Current Tracks and Pool retained.";
        case BANK_DEPENDENCY:
            return "Instrument or sample missing/unsupported. Current Track retained.";
        case BANK_AUDIO_BUSY:
            return "Audio stop was not acknowledged. Current Track retained.";
        case BANK_NO_BANK:
            return "Open or create a Bank first.";
        case BANK_EMPTY_SLOT:
            return "This Bank slot is empty.";
        case BANK_BAD_SLOT:
            return "Choose different source and destination slots.";
        case BANK_STALE:
            return "The Bank changed. Review the current slot and try again.";
        case BANK_CONFIRM_REQUIRED:
            return "Confirm replacement before recalling or changing an occupied slot.";
        default:
            return "Card operation failed. Check the saved destination before retrying; source "
                   "retained.";
    }
}
}  // namespace
void UIBankPage::onEnter(lv_obj_t* parent) {
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
    // A steady cursor keeps idle status polling free of redraws.
    lv_obj_set_style_anim_duration(input_, 0, LV_PART_CURSOR);
    lv_obj_set_style_anim_duration(input_, 0, LV_PART_CURSOR | LV_STATE_FOCUSED);
    lv_textarea_set_one_line(input_, true);
    lv_textarea_set_max_length(input_, 24 - 1);
    lv_textarea_set_accepted_chars(
        input_, "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789 -_");
    lv_obj_set_pos(input_, UI_MARGIN_X, UI_PROJECT_FILE_INPUT_Y);
    lv_obj_set_size(input_, UI_CONTENT_WIDTH - 2 * UI_MARGIN_X, UI_PROJECT_FILE_INPUT_H);
    lv_textarea_set_placeholder_text(input_,
                                     "Bank name: open existing, or use a new name for copies");
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
    lv_obj_add_flag(keyboard_, LV_OBJ_FLAG_HIDDEN);
    std::snprintf(
        message_,
        sizeof(message_),
        "Shift: Open / New / Save copy / Clear copy / Slot tools. Store copy saves the Track "
        "in a new named Bank. Preload pins Bank samples until unloaded.");
    alive_ = inter_mcu_backend_link_alive();
    read();
    timer_ = lv_timer_create(tick, 100, this);
    render();
}
void UIBankPage::onExit() {
    if (timer_)
        lv_timer_delete(timer_);
    timer_ = nullptr;
    if (root_)
        lv_obj_delete(root_);
    root_ = input_ = keyboard_ = hint_ = last_ = nullptr;
    pending_id_ = 0;
    slot_tools_ = false;
    source_slot_ = -1;
    valid_ = false;
    confirm_ = 0;
}
bool UIBankPage::ready() const {
    return alive_ && valid_ && static_cast<uint32_t>(lv_tick_get() - received_at_) < 1500 &&
           !status_.busy && !status_.blocked && !pending_id_;
}
void UIBankPage::read() {
    if (!alive_)
        return;
    BankOpMessage request;
    request.request_id = read_id_ = nextId();
    request.slot = slot_;
    request.track = getCurrentTrack();
    requested_at_ = lv_tick_get();
    inter_mcu_send_bank_op(request);
}
void UIBankPage::service() {
    const bool alive = inter_mcu_backend_link_alive();
    if (alive != alive_) {
        alive_ = alive;
        source_slot_ = -1;
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
    BankStatusMessage received;
    if (alive_ && inter_mcu_get_bank_status(&received) && received.request_id == read_id_ &&
        received.request_id != seen_read_id_ && received.slot == slot_ &&
        IsValidBankStatus(received)) {
        seen_read_id_ = received.request_id;
        received_at_ = lv_tick_get();
        const bool changed = !valid_ || std::memcmp(&status_, &received, sizeof(status_));
        if (confirm_ && received.revision != draft_.revision) {
            confirm_ = 0;
            std::snprintf(message_, sizeof(message_), "%s", errorText(BANK_STALE));
        }
        if (source_slot_ >= 0 && received.revision != source_revision_)
            source_slot_ = -1;
        const bool midi_completion =
            received.completed_op == BANK_PROGRAM_RECALL && received.completed_request_id &&
            (received.completed_request_id != status_.completed_request_id ||
             status_.completed_op != BANK_PROGRAM_RECALL);
        status_ = received;
        valid_ = true;
        if (pending_id_ && received.completed_request_id == pending_id_ &&
            received.completed_op == draft_.op) {
            pending_id_ = 0;
            std::snprintf(
                message_,
                sizeof(message_),
                "%s",
                received.error == BANK_OK && received.completed_op == BANK_PRELOAD
                    ? "Bank samples preloaded and pinned until unloaded. Tracks unchanged."
                    : errorText(received.error));
        }
        if (!pending_id_ && midi_completion)
            std::snprintf(message_,
                          sizeof(message_),
                          "%s",
                          received.error == BANK_OK ? "MIDI Program Change recall completed."
                                                    : errorText(received.error));
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
void UIBankPage::render() {
    char text[160];
    std::snprintf(
        text,
        sizeof(text),
        "Bank: %s    Slot %u / 128: %s    Target: Track %u",
        valid_ && status_.loaded ? status_.name : "none",
        slot_ + 1,
        valid_ && status_.occupied ? status_.instrument : (valid_ ? "empty" : "reading..."),
        trackDisplayNumber(getCurrentTrack()));
    if (slot_tools_)
        std::snprintf(
            text,
            sizeof(text),
            "Source: %u %s    Destination: %u %s\nBank: %s",
            source_slot_ >= 0 ? source_slot_ + 1 : 0,
            source_slot_ >= 0 ? source_name_ : "choose an occupied slot",
            slot_ + 1,
            valid_ && status_.occupied ? status_.instrument : (valid_ ? "empty" : "reading..."),
            valid_ && status_.loaded ? status_.name : "none");
    label(last_, text);
    label(hint_,
          !alive_                     ? "Audio engine disconnected"
          : valid_ && status_.busy    ? "Bank operation in progress..."
          : valid_ && status_.blocked ? "Another storage operation is busy."
                                      : message_);
    if (confirm_ || pending_id_ || (valid_ && status_.busy)) {
        lv_obj_add_state(input_, LV_STATE_DISABLED);
        lv_obj_add_state(keyboard_, LV_STATE_DISABLED);
    } else {
        lv_obj_remove_state(input_, LV_STATE_DISABLED);
        lv_obj_remove_state(keyboard_, LV_STATE_DISABLED);
    }
}
void UIBankPage::choose(uint8_t op) {
    if (!ready())
        return;
    if (op >= BANK_SAVE_COPY && !status_.loaded)
        return;
    if ((op == BANK_RECALL || op == BANK_CLEAR_COPY) && !status_.occupied)
        return;
    if (isTransfer(op) && !canTransfer())
        return;
    draft_ = BankOpMessage{};
    draft_.request_id = nextId();
    draft_.revision = status_.revision;
    draft_.op = op;
    draft_.slot = slot_;
    draft_.track = getCurrentTrack();
    if (op != BANK_RECALL && op != BANK_PRELOAD) {
        const auto* name = lv_textarea_get_text(input_);
        if (!WaveX::BankFile::ValidName(name)) {
            std::snprintf(message_, sizeof(message_), "%s", errorText(BANK_BAD_NAME));
            lv_obj_remove_flag(keyboard_, LV_OBJ_FLAG_HIDDEN);
            render();
            return;
        }
        detail::CopyWireString(draft_.name, sizeof(draft_.name), name);
    }
    if (isTransfer(op)) {
        transfer_draft_ = BankSlotOpMessage{};
        transfer_draft_.request_id = draft_.request_id;
        transfer_draft_.revision = draft_.revision;
        transfer_draft_.op = op;
        transfer_draft_.source_slot = static_cast<uint8_t>(source_slot_);
        transfer_draft_.destination_slot = slot_;
        detail::CopyWireString(transfer_draft_.name, sizeof(transfer_draft_.name), draft_.name);
    }
    if (isTransfer(op) || op == BANK_RECALL || op == BANK_STORE_COPY || op == BANK_CLEAR_COPY) {
        confirm_ = op;
        if (isTransfer(op))
            std::snprintf(message_,
                          sizeof(message_),
                          "%s slot %u to %u in new Bank '%s'? %s%s Original Bank retained.",
                          op == BANK_MOVE_SLOT ? "Move" : "Copy",
                          source_slot_ + 1,
                          slot_ + 1,
                          draft_.name,
                          status_.occupied ? "Destination replaced. " : "",
                          op == BANK_MOVE_SLOT ? "Source cleared in new Bank." : "Source kept.");
        else if (op == BANK_RECALL)
            std::snprintf(message_,
                          sizeof(message_),
                          "Recall slot %u to Track %u? Its current Instrument and unsaved sound "
                          "edits will be replaced. Track mix and MIDI stay unchanged.",
                          slot_ + 1,
                          trackDisplayNumber(draft_.track));
        else
            std::snprintf(message_,
                          sizeof(message_),
                          "%s slot %u in new Bank '%s'? The current Bank file is preserved. Track "
                          "sound edits are stored only by Store copy.",
                          op == BANK_STORE_COPY ? "Store Track into" : "Clear",
                          slot_ + 1,
                          draft_.name);
        lv_obj_add_flag(keyboard_, LV_OBJ_FLAG_HIDDEN);
        render();
        UINavigator::instance().refreshSoftkeys();
    } else
        send(op);
}
void UIBankPage::send(uint8_t op) {
    if (!ready() || draft_.op != op || draft_.track != getCurrentTrack())
        return;
    if (confirm_)
        draft_.flags = BANK_CONFIRM_REPLACE;
    confirm_ = 0;
    transfer_draft_.flags = draft_.flags;
    const auto result = isTransfer(op) ? inter_mcu_send_bank_slot_op(transfer_draft_)
                                       : inter_mcu_send_bank_op(draft_);
    if (result != ESP_OK)
        std::snprintf(message_, sizeof(message_), "Link busy. Try again.");
    else {
        pending_id_ = read_id_ = draft_.request_id;
        pending_at_ = requested_at_ = lv_tick_get();
        std::snprintf(message_, sizeof(message_), "Bank operation requested...");
        lv_obj_add_flag(keyboard_, LV_OBJ_FLAG_HIDDEN);
    }
    render();
    UINavigator::instance().refreshSoftkeys();
}
void UIBankPage::cancel() {
    confirm_ = 0;
    std::snprintf(message_, sizeof(message_), "Cancelled. Bank and Track are unchanged.");
    render();
    UINavigator::instance().refreshSoftkeys();
}
void UIBankPage::tick(lv_timer_t* timer) {
    static_cast<UIBankPage*>(lv_timer_get_user_data(timer))->service();
}
void UIBankPage::keyboardEvent(lv_event_t* event) {
    auto* self = static_cast<UIBankPage*>(lv_event_get_user_data(event));
    lv_obj_add_flag(self->keyboard_, LV_OBJ_FLAG_HIDDEN);
}
void UIBankPage::inputEvent(lv_event_t* event) {
    auto* self = static_cast<UIBankPage*>(lv_event_get_user_data(event));
    if (self->ready() && !self->confirm_)
        lv_obj_remove_flag(self->keyboard_, LV_OBJ_FLAG_HIDDEN);
}
void UIBankPage::move(int delta) {
    if (!ready() || confirm_)
        return;
    slot_ = static_cast<uint8_t>((slot_ + delta + 128) % 128);
    valid_ = false;
    read();
    render();
    UINavigator::instance().refreshSoftkeys();
}
bool UIBankPage::canTransfer() const {
    return ready() && status_.loaded && source_slot_ >= 0 && source_slot_ != slot_ &&
           source_revision_ == status_.revision;
}
void UIBankPage::markSource() {
    if (!ready() || !status_.occupied || confirm_)
        return;
    source_slot_ = slot_;
    source_revision_ = status_.revision;
    detail::CopyWireString(source_name_, sizeof(source_name_), status_.instrument);
    std::snprintf(
        message_,
        sizeof(message_),
        "Source marked. Choose a destination and a new Bank name, then Copy here or Move here.");
    render();
    UINavigator::instance().refreshSoftkeys();
}
void UIBankPage::slotTools(bool enabled) {
    slot_tools_ = enabled;
    lv_obj_set_style_text_font(last_, enabled ? UI_FONT_SMALL : UI_FONT_BODY, 0);
    source_slot_ = -1;
    std::snprintf(message_,
                  sizeof(message_),
                  "Mark an occupied Source, choose a destination and a new Bank name. Original "
                  "Bank retained.");
    if (enabled)
        markSource();
    UINavigator::instance().setShift(false);
    render();
    UINavigator::instance().refreshSoftkeys();
}
void UIBankPage::onTrackChanged() {
    confirm_ = 0;
    std::snprintf(message_,
                  sizeof(message_),
                  "Selected Track changed. Review the target before recalling or storing.");
    render();
    UINavigator::instance().refreshSoftkeys();
}
void UIBankPage::onInput(const InputEvent& event) {
    if (event.steps())
        move(event.steps() > 0 ? 1 : -1);
    else if (event.type == InputType::EncoderClick)
        choose(slot_tools_ ? BANK_COPY_SLOT : BANK_RECALL);
}
std::array<Softkey, NUM_SOFTKEYS> UIBankPage::getSoftkeys() {
    std::array<Softkey, NUM_SOFTKEYS> keys{};
    if (confirm_) {
        keys[0] = {"Cancel", [this] { cancel(); }};
        keys[1] = {"Confirm", [this] { send(confirm_); }, ready(), "Waiting for the audio engine"};
        return keys;
    }
    if (slot_tools_) {
        keys[0] = {"Back", [this] { slotTools(false); }};
        keys[1] = {"Source",
                   [this] { markSource(); },
                   ready() && status_.occupied,
                   "Select an occupied source slot"};
        keys[2] = {"Previous", [this] { move(-1); }, ready(), "Reading Bank"};
        keys[3] = {"Next", [this] { move(1); }, ready(), "Reading Bank"};
        keys[4] = {"Copy here",
                   [this] { choose(BANK_COPY_SLOT); },
                   canTransfer(),
                   "Mark a source and choose a different destination"};
        keys[5] = {"Move here",
                   [this] { choose(BANK_MOVE_SLOT); },
                   canTransfer(),
                   "Mark a source and choose a different destination"};
        return keys;
    }
    keys[0] = {"Back", [] { UINavigator::instance().pop(); }};
    keys[1] = {"Previous", [this] { move(-1); }, ready(), "Reading Bank"};
    keys[2] = {"Next", [this] { move(1); }, ready(), "Reading Bank"};
    keys[3] = {"Recall",
               [this] { choose(BANK_RECALL); },
               ready() && status_.occupied,
               "Select an occupied slot"};
    keys[4] = {"Store copy",
               [this] { choose(BANK_STORE_COPY); },
               ready() && status_.loaded,
               "Open a Bank first"};
    keys[5] = {"Preload",
               [this] { choose(BANK_PRELOAD); },
               ready() && status_.loaded,
               "Open a Bank first"};
    return keys;
}
std::array<Softkey, NUM_SOFTKEYS> UIBankPage::getShiftedSoftkeys() {
    if (confirm_ || slot_tools_)
        return getSoftkeys();
    std::array<Softkey, NUM_SOFTKEYS> keys{};
    keys[0] = {"Back", [] { UINavigator::instance().pop(); }};
    keys[1] = {"Open", [this] { choose(BANK_OPEN); }, ready(), "Reading Bank"};
    keys[2] = {"New", [this] { choose(BANK_NEW); }, ready(), "Reading Bank"};
    keys[3] = {"Save copy",
               [this] { choose(BANK_SAVE_COPY); },
               ready() && status_.loaded,
               "Open a Bank first"};
    keys[4] = {"Clear copy",
               [this] { choose(BANK_CLEAR_COPY); },
               ready() && status_.occupied,
               "Select an occupied slot"};
    keys[5] = {
        "Slot tools", [this] { slotTools(true); }, ready() && status_.loaded, "Open a Bank first"};
    return keys;
}
size_t UIBankPage::consoleState(char* out, size_t cap, size_t len) {
    using namespace WaveX::Debug;
    len = AppendKvInt(out, cap, len, "bankready", ready());
    len = AppendKvInt(out, cap, len, "bankbusy", status_.busy);
    len = AppendKvInt(out, cap, len, "bankpending", pending_id_ != 0);
    len = AppendKvInt(out, cap, len, "bankerror", status_.error);
    len = AppendKvInt(out, cap, len, "bankconfirm", confirm_);
    len = AppendKvInt(out, cap, len, "bankslot", slot_ + 1);
    len = AppendKvInt(out, cap, len, "banktools", slot_tools_);
    len = AppendKvInt(out, cap, len, "banksource", source_slot_ + 1);
    len = AppendKvInt(out, cap, len, "bankoccupied", valid_ && status_.occupied);
    return AppendKvText(out, cap, len, "bankname", valid_ ? status_.name : "");
}
bool UIBankPage::consoleCommand(const char* args, char* reply, size_t cap) {
    if (!args || std::strncmp(args, "NAME ", 5) || !input_ || !ready() || confirm_)
        return false;
    lv_textarea_set_text(input_, args + 5);
    std::snprintf(reply, cap, "ok");
    return true;
}
std::shared_ptr<UIPage> createBankPage() {
    return std::make_shared<UIBankPage>();
}
}  // namespace wavex_ui
