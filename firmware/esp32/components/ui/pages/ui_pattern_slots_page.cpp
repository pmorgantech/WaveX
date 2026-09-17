#include "ui/ui_pattern_slots_page.h"

#include <esp_random.h>

#include "../styles/ui_theme.h"
#include "debug/console_command.h"
#include "inter_mcu.h"
#include "ui/ui_navigator.h"
#include "ui/ui_pattern_files_page.h"

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
void label(lv_obj_t* obj, const char* value) {
    if (obj && std::strcmp(lv_label_get_text(obj), value))
        lv_label_set_text(obj, value);
}
const char* errorText(uint8_t error) {
    switch (error) {
        case SEQ_SLOT_OK:
            return "Done. Use Project Save copy to save all Pattern slots to the card.";
        case SEQ_SLOT_BUSY:
            return "Another operation is busy. Wait and try again.";
        case SEQ_SLOT_BAD_NAME:
            return "Use 1-23 letters/numbers, spaces, hyphens or underscores; no outer spaces.";
        case SEQ_SLOT_EMPTY:
            return "That slot is empty. Create a Pattern or copy the active Pattern here.";
        case SEQ_SLOT_EXISTS:
            return "That slot is occupied. Choose an empty destination.";
        case SEQ_SLOT_STOP_FIRST:
            return "Stop the sequencer first, including MIDI-armed playback. Then try again.";
        case SEQ_SLOT_CANCELLED:
            return "Queued launch cancelled by transport Stop / restart. The active Pattern is "
                   "unchanged.";
        case SEQ_SLOT_NO_MEMORY:
            return "Not enough sample memory for Project Patterns. Your working Pattern is "
                   "retained.";
        default:
            return "Pattern capture did not finish. Stop editing and try again.";
    }
}
}  // namespace
void UIPatternSlotsPage::onEnter(lv_obj_t* parent) {
    root_ = lv_obj_create(parent);
    ui_theme_apply_container_style(root_, false);
    lv_obj_set_size(root_, UI_CONTENT_WIDTH, UI_CONTENT_HEIGHT);
    lv_obj_set_style_pad_all(root_, 0, 0);
    lv_obj_remove_flag(root_, LV_OBJ_FLAG_SCROLLABLE);
    last_ = lv_label_create(root_);
    ui_theme_apply_label_style(last_, false);
    lv_obj_set_pos(last_, UI_MARGIN_X, UI_PADDING_SMALL);
    lv_obj_set_pos(last_, UI_SLOT_HEADING_X, UI_PADDING_SMALL);
    lv_obj_set_width(last_, UI_CONTENT_WIDTH - 2 * UI_SLOT_HEADING_X);
    for (int direction: {-1, 1}) {
        auto* button = lv_button_create(root_);
        ui_theme_apply_button_style(button, false);
        lv_obj_set_size(button, UI_SLOT_BUTTON_W, UI_SLOT_BUTTON_H);
        lv_obj_set_pos(
            button,
            direction < 0 ? UI_MARGIN_X : UI_CONTENT_WIDTH - UI_MARGIN_X - UI_SLOT_BUTTON_W,
            0);
        auto* text = lv_label_create(button);
        lv_label_set_text(text, direction < 0 ? "Previous" : "Next");
        lv_obj_center(text);
        lv_obj_add_event_cb(
            button,
            [](lv_event_t* e) {
                auto* page = static_cast<UIPatternSlotsPage*>(lv_event_get_user_data(e));
                page->move(lv_obj_get_x(static_cast<lv_obj_t*>(lv_event_get_current_target(e))) ==
                                   UI_MARGIN_X
                               ? -1
                               : 1);
            },
            LV_EVENT_CLICKED,
            this);
    }
    input_ = lv_textarea_create(root_);
    lv_textarea_set_one_line(input_, true);
    lv_textarea_set_max_length(input_, PROJECT_NAME_BYTES - 1);
    lv_textarea_set_accepted_chars(
        input_, "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789 -_");
    lv_obj_set_pos(input_, UI_MARGIN_X, UI_PROJECT_FILE_INPUT_Y);
    lv_obj_set_size(input_, UI_CONTENT_WIDTH - 2 * UI_MARGIN_X, UI_PROJECT_FILE_INPUT_H);
    lv_textarea_set_placeholder_text(input_, "Pattern name for Create / Copy / Rename");
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

    std::snprintf(message_,
                  sizeof(message_),
                  "Browse slots with Previous / Next or the encoder. Copy active preserves all "
                  "hidden steps and locks. Files imports into the active slot.");
    alive_ = inter_mcu_backend_link_alive();
    valid_ = false;
    soft_state_ = UINT32_MAX;
    read();
    timer_ = lv_timer_create(tick, 100, this);
    render();
}
void UIPatternSlotsPage::onExit() {
    if (timer_)
        lv_timer_delete(timer_);
    timer_ = nullptr;
    if (root_)
        lv_obj_delete(root_);
    root_ = input_ = keyboard_ = hint_ = last_ = nullptr;
    valid_ = false;
    pending_id_ = 0;
}
bool UIPatternSlotsPage::ready() const {
    return alive_ && valid_ && !status_.busy && !pending_id_ &&
           static_cast<uint32_t>(lv_tick_get() - received_at_) < 1500;
}
void UIPatternSlotsPage::read() {
    if (!alive_)
        return;
    SeqSlotOpMessage request;
    request.request_id = read_id_ = nextId();
    request.slot = slot_;
    requested_at_ = lv_tick_get();
    inter_mcu_send_seq_slot_op(request);
}
void UIPatternSlotsPage::service() {
    const bool alive = inter_mcu_backend_link_alive();
    if (alive != alive_) {
        alive_ = alive;
        valid_ = false;
        std::snprintf(
            message_,
            sizeof(message_),
            "Connection changed. Read the active slot before retrying an unconfirmed operation.");
        if (alive_)
            read();
    }
    SeqSlotStatusMessage received;
    if (alive_ && inter_mcu_get_seq_slot_status(&received) && received.request_id == read_id_ &&
        received.request_id != seen_id_ && received.slot == slot_ &&
        IsValidSeqSlotStatus(received)) {
        seen_id_ = received.request_id;
        received_at_ = lv_tick_get();
        status_ = received;
        valid_ = true;
        if (pending_id_ && received.completed_request_id == pending_id_) {
            pending_id_ = 0;
            std::snprintf(message_, sizeof(message_), "%s", errorText(received.error));
        }
    }
    if (valid_ && static_cast<uint32_t>(lv_tick_get() - received_at_) >= 1500)
        valid_ = false;
    if (pending_id_ && valid_ && !status_.busy &&
        static_cast<uint32_t>(lv_tick_get() - pending_at_) > 10000) {
        pending_id_ = 0;
        std::snprintf(
            message_,
            sizeof(message_),
            "Operation unconfirmed. Inspect this slot and the active Pattern before retrying.");
    }
    if (alive_ && static_cast<uint32_t>(lv_tick_get() - requested_at_) >= 300)
        read();
    render();
}
void UIPatternSlotsPage::move(int delta) {
    if (pending_id_)
        return;
    const auto next = static_cast<uint8_t>(std::clamp(static_cast<int>(slot_) + delta, 0, 127));
    if (next == slot_)
        return;
    slot_ = next;
    valid_ = false;
    read();
    render();
}
void UIPatternSlotsPage::send(uint8_t op) {
    if (!ready())
        return;
    const char* name = lv_textarea_get_text(input_);
    if (op != SEQ_SLOT_SELECT && op != SEQ_SLOT_LAUNCH && !WaveX::PatternFile::ValidName(name)) {
        std::snprintf(message_, sizeof(message_), "%s", errorText(SEQ_SLOT_BAD_NAME));
        render();
        return;
    }
    SeqSlotOpMessage request;
    request.request_id = nextId();
    request.op = op;
    request.slot = slot_;
    detail::CopyWireString(request.name, sizeof(request.name), name);
    if (inter_mcu_send_seq_slot_op(request) == ESP_OK) {
        pending_id_ = read_id_ = request.request_id;
        pending_at_ = requested_at_ = lv_tick_get();
        std::snprintf(message_, sizeof(message_), "Applying Pattern operation...");
    } else
        std::snprintf(message_, sizeof(message_), "Link busy. Try again.");
    render();
}
void UIPatternSlotsPage::render() {
    char value[112];
    if (valid_)
        std::snprintf(value,
                      sizeof(value),
                      "Slot %u: %s   |   Active: %u",
                      slot_ + 1,
                      status_.used ? status_.name : "Empty",
                      status_.active_pattern + 1);
    else
        std::snprintf(value, sizeof(value), "Slot %u: reading...", slot_ + 1);
    label(last_, value);
    if (alive_ && valid_ && status_.queued_pattern != 0xff) {
        char queued[192];
        std::snprintf(queued,
                      sizeof(queued),
                      "Pattern %u queued for the next loop. Back returns to the grid; Stop "
                      "cancels. Create / Copy / Rename require stopped playback.",
                      status_.queued_pattern + 1);
        label(hint_, queued);
    } else
        label(hint_,
              !alive_ ? "Audio engine disconnected"
                      : (valid_ && status_.busy ? "Pattern operation in progress..." : message_));
    const bool disabled = pending_id_ || (valid_ && status_.busy);
    for (auto* obj: {input_, keyboard_}) {
        if (lv_obj_has_state(obj, LV_STATE_DISABLED) != disabled) {
            if (disabled)
                lv_obj_add_state(obj, LV_STATE_DISABLED);
            else
                lv_obj_remove_state(obj, LV_STATE_DISABLED);
        }
    }
    uint32_t soft = (ready() ? 1 : 0) | (status_.used ? 2 : 0) | (pending_id_ ? 4 : 0);
    if (soft != soft_state_) {
        soft_state_ = soft;
        UINavigator::instance().refreshSoftkeys();
    }
}
std::array<Softkey, NUM_SOFTKEYS> UIPatternSlotsPage::getSoftkeys() {
    std::array<Softkey, NUM_SOFTKEYS> keys{};
    keys[0] = {"Back", [] { UINavigator::instance().pop(); }};
    keys[1] = {"Create",
               [this] { send(SEQ_SLOT_CREATE); },
               ready() && !status_.used,
               "Choose an empty slot"};
    keys[2] = {"Copy active",
               [this] { send(SEQ_SLOT_COPY); },
               ready() && !status_.used,
               "Choose an empty slot"};
    keys[3] = {"Rename",
               [this] { send(SEQ_SLOT_RENAME); },
               ready() && status_.used,
               "Choose an occupied slot"};
    keys[4] = {"Launch",
               [this] { send(SEQ_SLOT_LAUNCH); },
               ready() && status_.used,
               "Choose an occupied slot"};
    keys[5] = {"Files",
               [] { UINavigator::instance().push(createPatternFilesPage()); },
               !pending_id_,
               "Wait for the operation"};
    return keys;
}
void UIPatternSlotsPage::onInput(const InputEvent& event) {
    if (event.type == InputType::EncoderLeft || event.type == InputType::EncoderRight)
        move(event.steps());
    else
        UIPage::onInput(event);
}
void UIPatternSlotsPage::tick(lv_timer_t* timer) {
    static_cast<UIPatternSlotsPage*>(lv_timer_get_user_data(timer))->service();
}
void UIPatternSlotsPage::keyboardEvent(lv_event_t* e) {
    auto* self = static_cast<UIPatternSlotsPage*>(lv_event_get_user_data(e));
    lv_obj_add_flag(self->keyboard_, LV_OBJ_FLAG_HIDDEN);
}
void UIPatternSlotsPage::inputEvent(lv_event_t* e) {
    auto* self = static_cast<UIPatternSlotsPage*>(lv_event_get_user_data(e));
    if (self->ready())
        lv_obj_remove_flag(self->keyboard_, LV_OBJ_FLAG_HIDDEN);
}
size_t UIPatternSlotsPage::consoleState(char* out, size_t cap, size_t len) {
    using namespace WaveX::Debug;
    len = AppendKvInt(out, cap, len, "slot", slot_ + 1);
    len = AppendKvInt(out, cap, len, "slotready", ready());
    len = AppendKvInt(out, cap, len, "slotused", valid_ && status_.used);
    len = AppendKvInt(out, cap, len, "slotactive", valid_ ? status_.active_pattern + 1 : 0);
    len = AppendKvInt(out,
                      cap,
                      len,
                      "slotqueued",
                      valid_ && status_.queued_pattern < 128 ? status_.queued_pattern + 1 : 0);
    len = AppendKvInt(out, cap, len, "sloterror", status_.error);
    return AppendKvText(out, cap, len, "slotname", valid_ ? status_.name : "");
}
bool UIPatternSlotsPage::consoleCommand(const char* args, char* reply, size_t cap) {
    if (!args || !input_ || pending_id_)
        return false;
    if (!std::strncmp(args, "NAME ", 5))
        lv_textarea_set_text(input_, args + 5);
    else if (!std::strncmp(args, "SLOT ", 5)) {
        int slot = 0;
        char extra;
        if (std::sscanf(args + 5, "%d %c", &slot, &extra) != 1 || slot < 1 || slot > 128)
            return false;
        move(slot - 1 - slot_);
    } else
        return false;
    std::snprintf(reply, cap, "ok");
    return true;
}
std::shared_ptr<UIPage> createPatternSlotsPage() {
    return std::make_shared<UIPatternSlotsPage>();
}
}  // namespace wavex_ui
