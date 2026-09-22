#include "ui/ui_sample_record_page.h"

#include <esp_random.h>

#include "inter_mcu.h"
#include "ui/current_sample.h"
#include "ui/ui_navigator.h"
#include "ui/ui_pad_map_page.h"
#include "ui_theme.h"

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
const char* sources[] = {"Codec stereo", "Codec left", "Codec right", "Internal mix"};
const char* states[] = {
    "Ready to arm", "Armed", "Recording", "Finishing take", "Take ready", "Saving"};
const char* hints[] = {"Arm, then Start (manual) or wait for the threshold.",
                       "Press Start or wait for the threshold; Stop cancels an empty take.",
                       "Press Stop to finish the take.",
                       "Finishing the captured audio...",
                       "Audition, name and Save the take, or Discard it.",
                       "Writing the take to the card..."};
const char* errors[] = {"",
                        "Storage or take is busy",
                        "Take changed; read its current state",
                        "Action unavailable in this state",
                        "Not enough sample memory",
                        "Capture overflow; contiguous audio retained",
                        "Card write failed; take retained for retry",
                        "Name already exists; choose another",
                        "Use letters, numbers, spaces, - or _"};
void text(lv_obj_t* label, const char* value) {
    if (std::strcmp(lv_label_get_text(label), value))
        lv_label_set_text(label, value);
}
}  // namespace
void UISampleRecordPage::onEnter(lv_obj_t* parent) {
    root_ = lv_obj_create(parent);
    ui_theme_apply_container_style(root_, false);
    lv_obj_set_size(root_, UI_CONTENT_WIDTH, UI_CONTENT_HEIGHT);
    lv_obj_set_style_pad_all(root_, 0, 0);
    lv_obj_remove_flag(root_, LV_OBJ_FLAG_SCROLLABLE);
    const int width = (UI_CONTENT_WIDTH - 2 * UI_MARGIN_X - 4 * UI_GUTTER) / 5;
    const char* titles[] = {"SOURCE", "THRESHOLD", "PRE-ROLL", "MAX LENGTH", "MONITOR"};
    for (uint8_t i = 0; i < 5; ++i) {
        tiles_[i] = valueTileCreate(root_,
                                    UI_MARGIN_X + i * (width + UI_GUTTER),
                                    UI_PADDING_LARGE,
                                    width,
                                    UI_PERFORMANCE_TILE_HEIGHT,
                                    titles[i],
                                    "");
        valueTileSetOnAdjust(tiles_[i], [this, i](int delta) {
            focus_ = i;
            adjust(i, delta);
        });
    }
    for (uint8_t i = 0; i < 2; ++i) {
        meters_[i] = lv_bar_create(root_);
        lv_obj_set_pos(meters_[i], UI_MARGIN_X, UI_RECORD_METERS_Y + i * UI_RECORD_METER_GAP);
        lv_obj_set_size(meters_[i], UI_CONTENT_WIDTH - 2 * UI_MARGIN_X, UI_RECORD_METER_HEIGHT);
        lv_bar_set_range(meters_[i], 0, 32768);
        lv_obj_set_style_bg_color(meters_[i], UI_COLOR_CARD_ALT, LV_PART_MAIN);
        lv_obj_set_style_bg_color(meters_[i], UI_COLOR_ACCENT, LV_PART_INDICATOR);
    }
    input_ = lv_textarea_create(root_);
    lv_textarea_set_one_line(input_, true);
    lv_textarea_set_max_length(input_, FILE_NAME_MAX - 1);
    lv_textarea_set_accepted_chars(
        input_, "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789 -_");
    lv_textarea_set_placeholder_text(input_, "Take name for Save");
    lv_obj_set_pos(input_, UI_MARGIN_X, UI_RECORD_NAME_Y);
    lv_obj_set_size(input_, UI_CONTENT_WIDTH - 2 * UI_MARGIN_X, UI_RECORD_NAME_HEIGHT);
    lv_obj_set_style_bg_color(input_, UI_COLOR_CARD, 0);
    lv_obj_set_style_text_color(input_, UI_COLOR_FG, 0);
    lv_obj_set_style_text_font(input_, UI_FONT_BODY, 0);
    lv_obj_set_style_anim_duration(input_, 0, LV_PART_CURSOR);
    status_label_ = lv_label_create(root_);
    ui_theme_apply_label_style(status_label_, false);
    lv_obj_set_style_text_font(status_label_, UI_FONT_SMALL, 0);
    lv_obj_set_pos(status_label_, UI_MARGIN_X, UI_RECORD_STATUS_Y);
    lv_obj_set_width(status_label_, UI_CONTENT_WIDTH - 2 * UI_MARGIN_X);
    lv_label_set_long_mode(status_label_, LV_LABEL_LONG_WRAP);
    lv_obj_add_event_cb(
        input_,
        [](lv_event_t* e) {
            static_cast<UISampleRecordPage*>(lv_event_get_user_data(e))->showKeyboard();
        },
        LV_EVENT_CLICKED,
        this);
    alive_ = inter_mcu_backend_link_alive();
    valid_ = false;
    read();
    render();
    timer_ = lv_timer_create(
        [](lv_timer_t* timer) {
            static_cast<UISampleRecordPage*>(lv_timer_get_user_data(timer))->service();
        },
        67,
        this);
}
void UISampleRecordPage::showKeyboard() {
    if (keyboard_) {
        lv_obj_remove_flag(keyboard_, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    keyboard_ = lv_keyboard_create(root_);
    lv_keyboard_set_textarea(keyboard_, input_);
    lv_obj_set_size(keyboard_, UI_CONTENT_WIDTH - 2 * UI_MARGIN_X, UI_RECORD_KEYBOARD_HEIGHT);
    lv_obj_align(keyboard_, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(keyboard_, UI_COLOR_BG, 0);
    lv_obj_set_style_bg_color(keyboard_, UI_COLOR_CARD, LV_PART_ITEMS);
    lv_obj_set_style_text_color(keyboard_, UI_COLOR_FG, LV_PART_ITEMS);
    lv_obj_set_style_text_font(keyboard_, UI_FONT_BODY, LV_PART_ITEMS);
    auto close = [](lv_event_t* e) {
        auto* self = static_cast<UISampleRecordPage*>(lv_event_get_user_data(e));
        lv_obj_add_flag(self->keyboard_, LV_OBJ_FLAG_HIDDEN);
    };
    lv_obj_add_event_cb(keyboard_, close, LV_EVENT_READY, this);
    lv_obj_add_event_cb(keyboard_, close, LV_EVENT_CANCEL, this);
}

void UISampleRecordPage::onExit() {
    assignment_ = {};
    if (timer_)
        lv_timer_delete(timer_);
    timer_ = nullptr;
    if (root_)
        lv_obj_delete(root_);
    root_ = input_ = keyboard_ = status_label_ = nullptr;
    valid_ = false;
}
bool UISampleRecordPage::ready() const {
    return alive_ && valid_ && !pending_ && !status_.active_request_id &&
           lv_tick_get() - received_ < 1500;
}
void UISampleRecordPage::read() {
    RecordOpMessage query;
    query.request_id = nextId();
    read_id_ = query.request_id;
    inter_mcu_send_record_op(query);
    requested_ = lv_tick_get();
}
void UISampleRecordPage::send(uint8_t op) {
    if (!ready())
        return;
    auto request = config_;
    request.request_id = nextId();
    request.take_id = status_.take_id;
    request.op = op;
    if (op == REC_SAVE)
        detail::CopyWireString(request.name, sizeof(request.name), lv_textarea_get_text(input_));
    if (op == REC_DISCARD && status_.path[0])
        setCurrentSampleId(status_.sample_id);
    if (inter_mcu_send_record_op(request) == ESP_OK) {
        pending_ = request.request_id;
        keys_ = UINT32_MAX;  // The disabled pending row must refresh even when state is unchanged.
        std::snprintf(message_, sizeof(message_), "Waiting for confirmation...");
        read();
        render();
        UINavigator::instance().refreshSoftkeys();
    }
}
void UISampleRecordPage::service() {
    const bool alive = inter_mcu_backend_link_alive();
    if (alive != alive_) {
        alive_ = alive;
        valid_ = false;
        assignment_ = {};
        if (alive)
            read();
    }
    RecordStatusMessage received;
    if (alive_ && inter_mcu_get_record_status(&received) && received.request_id == read_id_ &&
        IsValidRecordStatus(received)) {
        status_ = received;
        received_ = lv_tick_get();
        valid_ = true;
        if (status_.state != REC_IDLE) {
            config_.source = status_.source;
            config_.max_frames = status_.max_frames;
            config_.threshold = status_.threshold;
            config_.preroll_ms = status_.preroll_ms;
            config_.monitor = status_.monitor;
        }
        if (assignment_.Complete(received)) {
            const auto assignment = assignment_;
            assignment_ = {};
            pending_ = 0;
            UINavigator::instance().push(
                createRecordedSampleMap(assignment.keyboard, assignment.sample));
            return;  // Navigation deletes this page's widgets/timer.
        }
        if (pending_ && received.completed_request_id == pending_) {
            assignment_ = {};
            pending_ = 0;
            std::snprintf(message_, sizeof(message_), "%s", errors[received.error]);
            if (received.completed_op == REC_SAVE && received.error == REC_OK) {
                setCurrentSampleId(received.sample_id);
                std::snprintf(message_,
                              sizeof(message_),
                              "Saved: %s. Shift: To pad / To keys, or Done to keep it in the Pool.",
                              received.path);
            }
        } else if (pending_ && received.active_request_id != pending_ && !received.take_id &&
                   received.state == REC_IDLE) {
            pending_ = 0;
            std::snprintf(message_,
                          sizeof(message_),
                          "Take unavailable. The previous action was not replayed.");
        }
    }
    if (alive_ && lv_tick_get() - requested_ >= 300)
        read();
    render();
    const uint32_t keys =
        uint32_t(ready()) | uint32_t(status_.state) << 1 | uint32_t(bool(status_.path[0])) << 5;
    if (keys != keys_) {
        keys_ = keys;
        UINavigator::instance().refreshSoftkeys();
    }
}
void UISampleRecordPage::render() {
    if (!root_)
        return;
    char value[48];
    valueTileSetValue(tiles_[0], sources[config_.source], true);
    if (config_.threshold)
        std::snprintf(value, sizeof(value), "%u%%", config_.threshold * 100u / 32768u);
    else
        std::strcpy(value, "Manual");
    valueTileSetValue(tiles_[1], value, true);
    std::snprintf(value, sizeof(value), "%u ms", config_.preroll_ms);
    valueTileSetValue(tiles_[2], value, true);
    std::snprintf(
        value, sizeof(value), "%lu s", static_cast<unsigned long>(config_.max_frames / 48000u));
    valueTileSetValue(tiles_[3], value, true);
    valueTileSetValue(tiles_[4],
                      config_.source == REC_INTERNAL_MIX ? "Internal"
                      : config_.monitor                  ? "On"
                                                         : "Off",
                      true);
    for (uint8_t i = 0; i < 5; ++i)
        valueTileSetFocus(tiles_[i], i == focus_);
    if (lv_bar_get_value(meters_[0]) != status_.rms_l)
        lv_bar_set_value(meters_[0], status_.rms_l, LV_ANIM_OFF);
    if (lv_bar_get_value(meters_[1]) != status_.rms_r)
        lv_bar_set_value(meters_[1], status_.rms_r, LV_ANIM_OFF);
    char text_buffer[768];
    std::snprintf(text_buffer,
                  sizeof(text_buffer),
                  "%s / %s / %.2f s / Peak %u:%u%% / Clips %lu\n%s\n%s\nCapture continues if you "
                  "leave this tab. Stop before "
                  "saving; an unsaved take is lost on power-off.",
                  !alive_   ? "Disconnected"
                  : !valid_ ? "Reading recorder"
                            : states[status_.state],
                  sources[status_.state == REC_IDLE ? config_.source : status_.source],
                  static_cast<double>(status_.frames) / 48000.0,
                  status_.peak_l * 100u / 32768u,
                  status_.peak_r * 100u / 32768u,
                  static_cast<unsigned long>(status_.clip_count),
                  status_.capture_error ? errors[status_.capture_error]
                  : status_.path[0]     ? "Saved. Shift: To pad / To keys, or Done."
                                        : hints[status_.state],
                  message_);
    text(status_label_, text_buffer);
}
void UISampleRecordPage::adjust(uint8_t field, int delta) {
    if (!ready() || status_.state != REC_IDLE || !delta)
        return;
    switch (field) {
        case 0:
            config_.source = static_cast<uint8_t>(std::clamp(int(config_.source) + delta, 0, 3));
            break;
        case 1:
            config_.threshold =
                static_cast<uint16_t>(std::clamp(int(config_.threshold) + delta * 328, 0, 32768));
            break;
        case 2:
            config_.preroll_ms =
                static_cast<uint16_t>(std::clamp(int(config_.preroll_ms) + delta * 10, 0, 500));
            break;
        case 3:
            config_.max_frames =
                static_cast<uint32_t>(std::clamp(int(config_.max_frames / 48000) + delta, 1, 120)) *
                48000;
            break;
        case 4:
            config_.monitor = delta > 0;
            break;
    }
    render();
}
void UISampleRecordPage::onInput(const InputEvent& e) {
    if (e.type == InputType::EncoderRight || e.type == InputType::EncoderLeft)
        adjust(focus_, e.steps());
    else if (e.type == InputType::EncoderClick) {
        focus_ = static_cast<uint8_t>((focus_ + 1) % 5);
        render();
    }
}
std::array<Softkey, NUM_SOFTKEYS> UISampleRecordPage::getSoftkeys() {
    const bool active =
        status_.state == REC_ARMED || status_.state == REC_CAPTURING || status_.state == REC_READY;
    const bool take = status_.state == REC_READY;
    std::array<Softkey, NUM_SOFTKEYS> keys{};
    keys[0] = {"Back", [] { UINavigator::instance().pop(); }};
    keys[1] = {active ? "Stop" : "Arm",
               [this, active] { send(active ? REC_STOP : REC_ARM); },
               ready() && (active || status_.state == REC_IDLE),
               "Finish the current take"};
    keys[2] = {
        "Start", [this] { send(REC_START); }, ready() && status_.state == REC_ARMED, "Arm first"};
    keys[3] = {"Audition",
               [this] { send(REC_AUDITION); },
               ready() && take && status_.frames >= 2,
               "Record a take first"};
    keys[4] = {
        "Save", [this] { send(REC_SAVE); }, ready() && take && !status_.path[0], "No unsaved take"};
    keys[5] = {status_.path[0] ? "Done" : "Discard",
               [this] { send(REC_DISCARD); },
               ready() && take,
               "Stop first"};
    return keys;
}
void UISampleRecordPage::assign(bool keyboard) {
    if (!ready() || status_.state != REC_READY || !status_.path[0])
        return;
    send(REC_DISCARD);
    if (pending_)
        assignment_.Begin(status_, pending_, keyboard);
}
std::array<Softkey, NUM_SOFTKEYS> UISampleRecordPage::getShiftedSoftkeys() {
    auto keys = getSoftkeys();
    const bool saved = ready() && status_.state == REC_READY && status_.path[0];
    keys[1] = {"To pad", [this] { assign(false); }, saved, "Save the take first"};
    keys[2] = {"To keys", [this] { assign(true); }, saved, "Save the take first"};
    return keys;
}
size_t UISampleRecordPage::consoleState(char* out, size_t cap, size_t len) {
    if (len >= cap)
        return len;
    const int n = std::snprintf(out + len,
                                cap - len,
                                " recready=%u recstate=%u recsource=%u recframes=%lu rectake=%lu "
                                "recsample=%u recerror=%u reccaptureerror=%u recpending=%lu "
                                "recsaved=%u recpeak=%u recclips=%lu",
                                ready(),
                                status_.state,
                                config_.source,
                                static_cast<unsigned long>(status_.frames),
                                static_cast<unsigned long>(status_.take_id),
                                status_.sample_id,
                                status_.error,
                                status_.capture_error,
                                static_cast<unsigned long>(pending_),
                                bool(status_.path[0]),
                                status_.peak_l,
                                static_cast<unsigned long>(status_.clip_count));
    return n > 0 ? len + static_cast<size_t>(n) : len;
}
bool UISampleRecordPage::consoleCommand(const char* args, char* reply, size_t cap) {
    unsigned source = 0;
    if (std::sscanf(args, "SOURCE %u", &source) == 1 && source <= REC_INTERNAL_MIX && ready() &&
        status_.state == REC_IDLE) {
        config_.source = static_cast<uint8_t>(source);
        render();
    } else if (!std::strncmp(args, "NAME ", 5))
        lv_textarea_set_text(input_, args + 5);
    else
        return false;
    std::snprintf(reply, cap, "OK");
    return true;
}
std::shared_ptr<UIPage> createSampleRecordPage() {
    return std::make_shared<UISampleRecordPage>();
}
}  // namespace wavex_ui
