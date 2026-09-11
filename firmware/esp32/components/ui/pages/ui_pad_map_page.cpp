#include "ui/ui_pad_map_page.h"

#include <esp_random.h>

#include "../styles/ui_theme.h"
#include "debug/console_command.h"
#include "inter_mcu.h"
#include "ui/current_track.h"
#include "ui/ui_navigator.h"
#include "ui/ui_pad_sound_page.h"

#include <algorithm>
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
void label(lv_obj_t* object, const char* value) {
    if (object && std::strcmp(lv_label_get_text(object), value))
        lv_label_set_text(object, value);
}
const char* errorText(uint8_t error) {
    switch (error) {
        case INST_ERROR_NONE:
            return "Done";
        case INST_ERROR_EXISTS:
            return "Name already exists. Save with a new name.";
        case INST_ERROR_BUSY:
            return "Instrument loader busy. Try again.";
        case INST_ERROR_MISSING_SAMPLES:
            return "Sample is no longer resident.";
        case INST_ERROR_IO:
            return "Card save failed. Existing copies are unchanged.";
        default:
            return "Cannot apply this edit.";
    }
}
}  // namespace
void UIPadMapPage::onEnter(lv_obj_t* parent) {
    root_ = lv_obj_create(parent);
    ui_theme_apply_container_style(root_, false);
    lv_obj_set_size(root_, UI_CONTENT_WIDTH, UI_CONTENT_HEIGHT);
    lv_obj_set_style_pad_all(root_, 0, 0);
    lv_obj_remove_flag(root_, LV_OBJ_FLAG_SCROLLABLE);
    for (uint8_t i = 0; i < 16; ++i) {
        auto& b = pads_[i];
        b.owner = this;
        b.index = i;
        b.object = lv_button_create(root_);
        ui_theme_apply_button_style(b.object, false);
        lv_obj_set_pos(b.object, UI_MARGIN_X + (i % 4) * 220, UI_PADDING_MEDIUM + (i / 4) * 130);
        lv_obj_set_size(b.object, 210, 120);
        b.label = lv_label_create(b.object);
        lv_obj_set_width(b.label, 184);
        lv_obj_set_style_text_font(b.label, UI_FONT_SMALL, 0);
        lv_label_set_long_mode(b.label, LV_LABEL_LONG_WRAP);
        lv_obj_center(b.label);
        lv_obj_add_event_cb(b.object, padEvent, LV_EVENT_CLICKED, &b);
    }
    title_ = lv_label_create(root_);
    ui_theme_apply_label_style(title_, false);
    lv_obj_set_pos(title_, 920, 20);
    lv_obj_set_width(title_, 330);
    lv_label_set_long_mode(title_, LV_LABEL_LONG_WRAP);
    status_ = lv_label_create(root_);
    ui_theme_apply_label_style(status_, false);
    lv_obj_set_style_text_font(status_, UI_FONT_SMALL, 0);
    lv_obj_set_pos(status_, 920, 140);
    lv_obj_set_width(status_, 330);
    lv_label_set_long_mode(status_, LV_LABEL_LONG_WRAP);
    timer_ = lv_timer_create(tick, 100, this);
    onTrackChanged();
}
void UIPadMapPage::onExit() {
    if (timer_)
        lv_timer_delete(timer_);
    timer_ = nullptr;
    if (root_)
        lv_obj_delete(root_);
    root_ = overlay_ = input_ = name_hint_ = status_ = title_ = nullptr;
    valid_ = false;
    pending_id_ = 0;
    view_ = View::Pads;
}
void UIPadMapPage::onTrackChanged() {
    closeOverlay();
    valid_ = false;
    pending_id_ = 0;
    alive_ = inter_mcu_backend_link_alive();
    std::snprintf(message_, sizeof(message_), "Reading Instrument...");
    read();
    render();
}
bool UIPadMapPage::editable() const {
    return alive_ && valid_ && map_.editable && !map_.busy && !pending_id_;
}
void UIPadMapPage::read() {
    if (!alive_)
        return;
    read_id_ = nextId();
    requested_at_ = lv_tick_get();
    inter_mcu_send_instrument_edit({read_id_, getCurrentTrack(), INST_OP_GET_PAD_MAP, ""});
    if (view_ == View::Samples)
        inter_mcu_request_sample_meta_page(sample_first_, 8);
}
bool UIPadMapPage::send(InstOpMessage request) {
    if (!alive_ || pending_id_)
        return false;
    request.slot = getCurrentTrack();
    request.request_id = nextId();
    if (inter_mcu_send_instrument_edit(request) != ESP_OK) {
        std::snprintf(message_, sizeof(message_), "Link busy. Try again.");
        render();
        return false;
    }
    pending_id_ = request.request_id;
    read_id_ = request.request_id;
    pending_at_ = requested_at_ = lv_tick_get();
    std::snprintf(message_, sizeof(message_), "Applying...");
    render();
    UINavigator::instance().refreshSoftkeys();
    return true;
}
void UIPadMapPage::service() {
    const bool alive = inter_mcu_backend_link_alive();
    if (alive != alive_) {
        alive_ = alive;
        valid_ = false;
        pending_id_ = 0;
        closeOverlay();
        std::snprintf(message_,
                      sizeof(message_),
                      alive ? "Reading Instrument..." : "Audio engine disconnected");
        if (alive)
            read();
        UINavigator::instance().refreshSoftkeys();
    }
    InstZoneSyncMessage received;
    if (alive_ && inter_mcu_get_instrument_map(&received) && received.request_id == read_id_ &&
        received.track == getCurrentTrack() && received.loaded <= 1 && received.editable <= 1 &&
        received.busy <= 1 && received.mode <= 1 &&
        std::memchr(received.name, 0, sizeof(received.name))) {
        const bool changed = !valid_ || std::memcmp(&map_, &received, sizeof(map_)) != 0;
        map_ = received;
        valid_ = true;
        if (pending_id_ && received.completed_request_id == pending_id_) {
            pending_id_ = 0;
            std::snprintf(message_, sizeof(message_), "%s", errorText(received.error));
        } else if (!message_[0] || std::strcmp(message_, "Reading Instrument...") == 0) {
            std::snprintf(
                message_,
                sizeof(message_),
                "%s",
                map_.editable
                    ? "Tap a pad to select and audition.\nAssign chooses a resident sample."
                    : "Choose New kit to create a drum Instrument on this Track.");
        }
        if (changed)
            UINavigator::instance().refreshSoftkeys();
    }
    if (alive_ && valid_ && map_.editable) {
        uint8_t requested = 0;
        for (const auto& pad: map_.pads) {
            SampleMetadata meta;
            if (pad.sample_id && !inter_mcu_get_sample_meta(pad.sample_id, &meta)) {
                inter_mcu_request_sample_meta(pad.sample_id);
                if (++requested == 2)
                    break;  // bound metadata traffic after a reload
            }
        }
    }
    if (pending_id_ && static_cast<uint32_t>(lv_tick_get() - pending_at_) > 5000) {
        pending_id_ = 0;
        std::snprintf(
            message_, sizeof(message_), "Edit not confirmed. Check the map before retrying.");
        UINavigator::instance().refreshSoftkeys();
    }
    if (alive_ && static_cast<uint32_t>(lv_tick_get() - requested_at_) >= 300)
        read();
    render();
}
void UIPadMapPage::render() {
    if (!root_)
        return;
    char text[192];
    std::snprintf(text,
                  sizeof(text),
                  "Track %u / %s",
                  trackDisplayNumber(getCurrentTrack()),
                  valid_ && map_.name[0] ? map_.name : "Empty");
    if (std::strcmp(context_, text)) {
        std::snprintf(
            context_, sizeof(context_), "%.*s", static_cast<int>(sizeof(context_) - 1), text);
        UINavigator::instance().refreshContext();
    }
    std::snprintf(text,
                  sizeof(text),
                  "Pad %u\nChoke %u",
                  selected_ + 1,
                  valid_ ? map_.pads[selected_].choke_group : 0);
    label(title_, text);
    label(status_, !alive_ ? "Audio engine disconnected" : message_);
    for (uint8_t i = 0; i < 16; ++i) {
        SampleMetadata meta;
        const uint16_t id = valid_ && map_.editable ? map_.pads[i].sample_id : uint16_t{0};
        const bool known = id && inter_mcu_get_sample_meta(id, &meta);
        const char* name = known ? std::strrchr(meta.name, '/') : nullptr;
        std::snprintf(text,
                      sizeof(text),
                      "Pad %u / %u\n%s",
                      i + 1,
                      INST_PAD_FIRST_NOTE + i,
                      id ? (known ? (name ? name + 1 : meta.name) : "Sample") : "Empty");
        label(pads_[i].label, text);
        lv_obj_set_style_bg_color(
            pads_[i].object, selected_ == i ? UI_COLOR_ACCENT : UI_COLOR_CARD_ALT, 0);
    }
    if (view_ == View::Samples) {
        SampleMetadata page[8];
        uint16_t total = 0, first = 0;
        const auto count = inter_mcu_get_sample_meta_page(page, 8, &total, &first);
        if (first == sample_first_) {
            if (sample_total_ != total) {
                sample_total_ = total;
                UINavigator::instance().refreshSoftkeys();
            }
            for (uint8_t i = 0; i < 8; ++i) {
                auto& b = choices_[i];
                b.sample = i < count ? page[i].sample_id : 0;
                const char* base = i < count ? std::strrchr(page[i].name, '/') : nullptr;
                std::snprintf(
                    text, sizeof(text), "%s", i < count ? (base ? base + 1 : page[i].name) : "--");
                label(b.label, text);
                if (b.sample)
                    lv_obj_remove_state(b.object, LV_STATE_DISABLED);
                else
                    lv_obj_add_state(b.object, LV_STATE_DISABLED);
            }
        }
    }
}
void UIPadMapPage::select(uint8_t pad, bool audition) {
    if (pad >= INST_PAD_COUNT)
        return;
    selected_ = pad;
    if (audition && editable() && map_.pads[pad].sample_id)
        inter_mcu_send_note_on_track(INST_PAD_FIRST_NOTE + pad, 100, getCurrentTrack());
    render();
    UINavigator::instance().refreshSoftkeys();
}
void UIPadMapPage::setPad(uint16_t sample, uint8_t choke) {
    if (!editable())
        return;
    InstOpMessage request;
    request.op = INST_OP_SET_PAD_SAMPLE;
    request.pad_index = selected_;
    request.pad_sample_id = sample;
    request.pad_choke = choke;
    send(request);
}
void UIPadMapPage::closeOverlay() {
    if (overlay_)
        lv_obj_delete(overlay_);
    overlay_ = input_ = name_hint_ = nullptr;
    view_ = View::Pads;
    for (auto& b: choices_)
        b = Button{};
    if (root_)
        UINavigator::instance().refreshSoftkeys();
}
void UIPadMapPage::showNames(uint8_t op) {
    closeOverlay();
    view_ = View::Names;
    name_op_ = op;
    overlay_ = lv_obj_create(root_);
    ui_theme_apply_container_style(overlay_, false);
    lv_obj_set_pos(overlay_, 0, 0);
    lv_obj_set_size(overlay_, UI_CONTENT_WIDTH, UI_CONTENT_HEIGHT);
    lv_obj_remove_flag(overlay_, LV_OBJ_FLAG_SCROLLABLE);
    input_ = lv_textarea_create(overlay_);
    lv_textarea_set_one_line(input_, true);
    lv_textarea_set_max_length(input_, INST_NAME_BYTES - 1);
    lv_textarea_set_accepted_chars(
        input_, "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789 -_");
    lv_obj_set_pos(input_, 20, 20);
    lv_obj_set_width(input_, 1220);
    char initial[INST_NAME_BYTES];
    std::snprintf(initial, sizeof(initial), "Kit %u", trackDisplayNumber(getCurrentTrack()));
    lv_textarea_set_text(input_, op == INST_OP_NEW || !map_.name[0] ? initial : map_.name);
    auto* hint = lv_label_create(overlay_);
    name_hint_ = hint;
    lv_obj_set_pos(hint, 20, 100);
    lv_label_set_text(hint,
                      op == INST_OP_SAVE
                          ? "Save a new copy in wavex/instruments. Use a new name for each copy."
                          : "Name: 1-23 letters, numbers, spaces, hyphens or underscores.");
    auto* keyboard = lv_keyboard_create(overlay_);
    lv_obj_set_size(keyboard, 1240, 360);
    lv_obj_align(keyboard, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_keyboard_set_textarea(keyboard, input_);
    lv_obj_add_event_cb(keyboard, keyboardEvent, LV_EVENT_READY, this);
    lv_obj_add_event_cb(keyboard, keyboardEvent, LV_EVENT_CANCEL, this);
    UINavigator::instance().refreshSoftkeys();
}
void UIPadMapPage::acceptName() {
    if (!input_)
        return;
    const char* name = lv_textarea_get_text(input_);
    if (!IsValidInstrumentName(name)) {
        lv_label_set_text(name_hint_,
                          "Use 1-23 letters/numbers with spaces, hyphens or underscores; no "
                          "leading/trailing spaces.");
        return;
    }
    InstOpMessage request(0, getCurrentTrack(), name_op_, name);
    closeOverlay();
    send(request);
}
void UIPadMapPage::showSamples(uint16_t first) {
    closeOverlay();
    view_ = View::Samples;
    sample_first_ = first;
    overlay_ = lv_obj_create(root_);
    ui_theme_apply_container_style(overlay_, false);
    lv_obj_set_size(overlay_, UI_CONTENT_WIDTH, UI_CONTENT_HEIGHT);
    lv_obj_set_style_pad_all(overlay_, 0, 0);
    lv_obj_remove_flag(overlay_, LV_OBJ_FLAG_SCROLLABLE);
    auto* hint = lv_label_create(overlay_);
    lv_obj_set_pos(hint, 20, 15);
    lv_label_set_text(hint, "Choose a resident sample. Load more from Sample > Browse.");
    for (uint8_t i = 0; i < 8; ++i) {
        auto& b = choices_[i];
        b.owner = this;
        b.index = i;
        b.object = lv_button_create(overlay_);
        ui_theme_apply_button_style(b.object, false);
        lv_obj_set_pos(b.object, 20 + (i % 2) * 620, 65 + (i / 2) * 112);
        lv_obj_set_size(b.object, 608, 100);
        b.label = lv_label_create(b.object);
        lv_obj_set_width(b.label, 568);
        lv_label_set_long_mode(b.label, LV_LABEL_LONG_DOT);
        lv_obj_center(b.label);
        lv_label_set_text(b.label, "Reading samples...");
        lv_obj_add_state(b.object, LV_STATE_DISABLED);
        lv_obj_add_event_cb(b.object, sampleEvent, LV_EVENT_CLICKED, &b);
    }
    inter_mcu_request_sample_meta_page(first, 8);
    UINavigator::instance().refreshSoftkeys();
}
void UIPadMapPage::padEvent(lv_event_t* e) {
    auto* b = static_cast<Button*>(lv_event_get_user_data(e));
    b->owner->select(b->index, true);
}
void UIPadMapPage::sampleEvent(lv_event_t* e) {
    auto* b = static_cast<Button*>(lv_event_get_user_data(e));
    auto* page = b->owner;
    const auto sample = b->sample;
    const auto choke = page->map_.pads[page->selected_].choke_group;
    page->closeOverlay();
    page->setPad(sample, choke);
}
void UIPadMapPage::keyboardEvent(lv_event_t* e) {
    auto* page = static_cast<UIPadMapPage*>(lv_event_get_user_data(e));
    if (lv_event_get_code(e) == LV_EVENT_READY)
        page->acceptName();
    else
        page->closeOverlay();
}
void UIPadMapPage::tick(lv_timer_t* t) {
    static_cast<UIPadMapPage*>(lv_timer_get_user_data(t))->service();
}
void UIPadMapPage::onInput(const InputEvent& e) {
    if (view_ != View::Pads)
        return;
    if (e.type == InputType::EncoderLeft || e.type == InputType::EncoderRight)
        select(static_cast<uint8_t>(std::clamp(selected_ + e.steps(), 0, 15)), false);
    else if (e.type == InputType::ButtonPress || e.type == InputType::EncoderClick)
        select(selected_, true);
}
std::array<Softkey, NUM_SOFTKEYS> UIPadMapPage::getSoftkeys() {
    std::array<Softkey, NUM_SOFTKEYS> keys{};
    if (view_ != View::Pads) {
        keys[0] = {"Cancel", [this] { closeOverlay(); }};
        if (view_ == View::Names)
            keys[1] = {"Confirm", [this] { acceptName(); }};
        else if (view_ == View::ConfirmNew)
            keys[1] = {"Confirm", [this] { showNames(INST_OP_NEW); }};
        else {
            keys[1] = {"Previous",
                       [this] { showSamples(sample_first_ - 8); },
                       sample_first_ >= 8,
                       "First page"};
            keys[2] = {"Next",
                       [this] { showSamples(sample_first_ + 8); },
                       sample_first_ + 8 < sample_total_,
                       "Last page"};
        }
        return keys;
    }
    keys[0] = {"Back", [] { UINavigator::instance().pop(); }};
    keys[1] = {"Assign", [this] { showSamples(); }, editable(), "Create a kit first"};
    keys[2] = {
        "Choke -",
        [this] { setPad(map_.pads[selected_].sample_id, map_.pads[selected_].choke_group - 1); },
        editable() && map_.pads[selected_].sample_id && map_.pads[selected_].choke_group > 0,
        "No choke"};
    keys[3] = {
        "Choke +",
        [this] { setPad(map_.pads[selected_].sample_id, map_.pads[selected_].choke_group + 1); },
        editable() && map_.pads[selected_].sample_id && map_.pads[selected_].choke_group < 15,
        "Assign a sample first"};
    keys[4] = {"Save copy",
               [this] { showNames(INST_OP_SAVE); },
               alive_ && valid_ && map_.loaded && !map_.busy && !pending_id_,
               "No Instrument to save"};
    keys[5] = {"New kit",
               [this] {
                   view_ = View::ConfirmNew;
                   std::snprintf(
                       message_,
                       sizeof(message_),
                       "Replace Track %u's Instrument? Other Tracks and pattern steps remain.",
                       trackDisplayNumber(getCurrentTrack()));
                   render();
                   UINavigator::instance().refreshSoftkeys();
               },
               alive_ && valid_ && !map_.busy && !pending_id_,
               "Waiting for the audio engine"};
    return keys;
}
std::array<Softkey, NUM_SOFTKEYS> UIPadMapPage::getShiftedSoftkeys() {
    if (view_ != View::Pads)
        return getSoftkeys();
    std::array<Softkey, NUM_SOFTKEYS> keys{};
    keys[0] = {"Back", [] { UINavigator::instance().pop(); }};
    keys[1] = {"Rename",
               [this] { showNames(INST_OP_SET_NAME); },
               valid_ && map_.loaded && !map_.busy && !pending_id_,
               "No Instrument"};
    keys[2] = {"Clear pad", [this] { setPad(0, 0); }, editable(), "Create a kit first"};
    keys[3] = {"Sound",
               [this] { UINavigator::instance().push(createPadSoundPage(selected_)); },
               editable() && map_.pads[selected_].sample_id,
               "Assign a sample first"};
    keys[4] = {"Track -",
               [this] {
                   setCurrentTrack(getCurrentTrack() - 1);
                   onTrackChanged();
               },
               getCurrentTrack() > 0,
               "First Track"};
    keys[5] = {"Track +",
               [this] {
                   setCurrentTrack(getCurrentTrack() + 1);
                   onTrackChanged();
               },
               getCurrentTrack() < 15,
               "Last Track"};
    return keys;
}
size_t UIPadMapPage::consoleState(char* out, size_t cap, size_t len) {
    using namespace WaveX::Debug;
    len = AppendKvInt(out, cap, len, "kitready", valid_ && alive_ && !map_.busy && !pending_id_);
    len = AppendKvInt(out, cap, len, "kiteditable", valid_ && map_.editable);
    len = AppendKvInt(out, cap, len, "kitpad", selected_ + 1);
    len = AppendKvInt(out, cap, len, "kitsample", valid_ ? map_.pads[selected_].sample_id : 0);
    len = AppendKvInt(out, cap, len, "kitchoke", valid_ ? map_.pads[selected_].choke_group : 0);
    len = AppendKvInt(out, cap, len, "kiterror", map_.error);
    len = AppendKvInt(out, cap, len, "kitview", static_cast<int>(view_));
    len = AppendKvInt(out, cap, len, "kitpick0", choices_[0].sample);
    return AppendKvText(out, cap, len, "kitname", valid_ ? map_.name : "");
}
bool UIPadMapPage::consoleCommand(const char* args, char* reply, size_t cap) {
    int a = 0;
    if (args && std::strncmp(args, "NAME ", 5) == 0 && input_)
        lv_textarea_set_text(input_, args + 5);
    else if (args && std::sscanf(args, "PAD %d", &a) == 1 && a >= 1 && a <= 16)
        lv_obj_send_event(pads_[a - 1].object, LV_EVENT_CLICKED, nullptr);
    else if (args && std::sscanf(args, "CHOOSE %d", &a) == 1 && view_ == View::Samples && a >= 1 &&
             a <= 8 && choices_[a - 1].sample)
        lv_obj_send_event(choices_[a - 1].object, LV_EVENT_CLICKED, nullptr);
    else
        return false;
    std::snprintf(reply, cap, "ok");
    return true;
}
std::shared_ptr<UIPage> createPadMapPage() {
    return std::make_shared<UIPadMapPage>();
}
}  // namespace wavex_ui
