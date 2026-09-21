#include "ui/ui_sample_file_page.h"

#include <esp_random.h>

#include "inter_mcu.h"
#include "ui/ui_navigator.h"
#include "ui_theme.h"

#include <cstdio>
#include <cstring>

namespace wavex_ui {
namespace {
using namespace WaveX::Protocol;
uint32_t nextId() {
    static uint32_t id = esp_random();
    if (!++id)
        ++id;
    return id;
}
const char* errorText(uint8_t error) {
    switch (error) {
        case SAMPLE_FILE_OK:
            return "Saved";
        case SAMPLE_FILE_BUSY:
            return "Storage is busy. Wait for the current operation.";
        case SAMPLE_FILE_BAD_SAMPLE:
            return "Sample is unavailable. Return to the editor and select it again.";
        case SAMPLE_FILE_BAD_NAME:
            return "Use letters, numbers, spaces, hyphens or underscores; no outer spaces.";
        case SAMPLE_FILE_EXISTS:
            return "Name or temporary file already exists. Choose another name.";
        case SAMPLE_FILE_NO_SPACE:
            return "Not enough free card space.";
        case SAMPLE_FILE_CHANGED:
            return "WAV or saved edits have changed. Reload and check the sample.";
        default:
            return "Card operation failed. Check the card and retry; use a new name for a copy.";
    }
}
class SampleFilePage : public UIPage {
   public:
    SampleFilePage(uint16_t id, bool copy) : sample_id_(id), copy_(copy) {}
    const char* name() const override { return copy_ ? "Save Sample As" : "Save Sample Edits"; }
    void onEnter(lv_obj_t* parent) override {
        root_ = lv_obj_create(parent);
        ui_theme_apply_container_style(root_, false);
        lv_obj_set_size(root_, UI_CONTENT_WIDTH, UI_CONTENT_HEIGHT);
        lv_obj_set_style_pad_all(root_, 0, 0);
        lv_obj_remove_flag(root_, LV_OBJ_FLAG_SCROLLABLE);
        hint_ = lv_label_create(root_);
        ui_theme_apply_label_style(hint_, false);
        lv_obj_set_pos(hint_, UI_MARGIN_X, UI_PADDING_SMALL);
        lv_obj_set_width(hint_, UI_CONTENT_WIDTH - 2 * UI_MARGIN_X);
        lv_obj_set_height(hint_, UI_PROJECT_FILE_INPUT_Y - UI_PADDING_SMALL);
        lv_label_set_long_mode(hint_, LV_LABEL_LONG_DOT);
        if (copy_) {
            input_ = lv_textarea_create(root_);
            lv_textarea_set_one_line(input_, true);
            lv_textarea_set_max_length(input_, FILE_NAME_MAX - 1);
            lv_textarea_set_accepted_chars(
                input_, "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789 -_");
            lv_textarea_set_placeholder_text(input_, "New sample name (without .wav)");
            lv_obj_set_pos(input_, UI_MARGIN_X, UI_PROJECT_FILE_INPUT_Y);
            lv_obj_set_size(input_, UI_CONTENT_WIDTH - 2 * UI_MARGIN_X, UI_PROJECT_FILE_INPUT_H);
            lv_obj_set_style_bg_color(input_, UI_COLOR_CARD, 0);
            lv_obj_set_style_text_color(input_, UI_COLOR_FG, 0);
            lv_obj_set_style_text_font(input_, UI_FONT_BODY, 0);
            auto* keyboard = lv_keyboard_create(root_);
            lv_obj_set_size(
                keyboard, UI_CONTENT_WIDTH - 2 * UI_PADDING_SMALL, UI_PROJECT_FILE_KEYBOARD_H);
            lv_obj_align(keyboard, LV_ALIGN_BOTTOM_MID, 0, 0);
            lv_obj_set_style_bg_color(keyboard, UI_COLOR_BG, 0);
            lv_obj_set_style_bg_color(keyboard, UI_COLOR_CARD, LV_PART_ITEMS);
            lv_obj_set_style_text_color(keyboard, UI_COLOR_FG, LV_PART_ITEMS);
            lv_obj_set_style_text_font(keyboard, UI_FONT_BODY, LV_PART_ITEMS);
            lv_keyboard_set_textarea(keyboard, input_);
            lv_obj_add_event_cb(
                keyboard,
                [](lv_event_t* e) {
                    static_cast<SampleFilePage*>(lv_event_get_user_data(e))->save();
                },
                LV_EVENT_READY,
                this);
        }
        std::snprintf(message_,
                      sizeof(message_),
                      "%s",
                      copy_
                          ? "Copy the complete WAV and its edits to a new name in the same folder."
                          : "Save sample-wide defaults: trim, loop, gain, fades and channels. "
                            "Saved Projects keep their own edits.");
        alive_ = inter_mcu_backend_link_alive();
        read();
        timer_ = lv_timer_create(
            [](lv_timer_t* t) {
                static_cast<SampleFilePage*>(lv_timer_get_user_data(t))->service();
            },
            100,
            this);
        render();
    }
    void onExit() override {
        if (timer_)
            lv_timer_delete(timer_);
        timer_ = nullptr;
        if (root_)
            lv_obj_delete(root_);
        root_ = input_ = hint_ = nullptr;
        valid_ = false;
        pending_ = 0;
    }
    std::array<Softkey, NUM_SOFTKEYS> getSoftkeys() override {
        std::array<Softkey, NUM_SOFTKEYS> keys{};
        keys[0] = {"Back", [] { UINavigator::instance().pop(); }};
        keys[1] = {copy_ ? "Save copy" : "Save edits",
                   [this] { save(); },
                   ready(),
                   "Waiting for file status"};
        return keys;
    }
    std::array<Softkey, NUM_SOFTKEYS> getShiftedSoftkeys() override { return getSoftkeys(); }
    size_t consoleState(char* out, size_t cap, size_t len) override {
        if (len >= cap)
            return len;
        const int n = std::snprintf(
            out + len,
            cap - len,
            " samplefileid=%u fileready=%u filebusy=%u fileprogress=%u filepending=%lu "
            "fileerror=%u filecompleted=%lu",
            sample_id_,
            ready(),
            status_.busy,
            status_.progress,
            static_cast<unsigned long>(pending_),
            status_.error,
            static_cast<unsigned long>(status_.completed_request_id));
        return n > 0 ? len + static_cast<size_t>(n) : len;
    }
    bool consoleCommand(const char* args, char* reply, size_t cap) override {
        if (copy_ && input_ && !std::strncmp(args, "NAME ", 5)) {
            lv_textarea_set_text(input_, args + 5);
        } else if (!std::strcmp(args, "SAVE")) {
            save();
        } else
            return false;
        std::snprintf(reply, cap, "ok");
        return true;
    }

   private:
    bool ready() const {
        return alive_ && valid_ && !pending_ && !status_.busy &&
               uint32_t(lv_tick_get() - received_) < 1500;
    }
    void read() {
        if (!alive_)
            return;
        SampleFileOpMessage request;
        request.request_id = read_id_ = nextId();
        requested_ = lv_tick_get();
        inter_mcu_send_sample_file_op(request);
    }
    void save() {
        if (!ready())
            return;
        SampleFileOpMessage request;
        request.request_id = nextId();
        request.sample_id = sample_id_;
        request.op = copy_ ? SAMPLE_FILE_COPY : SAMPLE_FILE_SAVE;
        if (copy_)
            std::snprintf(request.name, sizeof(request.name), "%s", lv_textarea_get_text(input_));
        if (inter_mcu_send_sample_file_op(request) != ESP_OK) {
            std::snprintf(message_, sizeof(message_), "Could not send save request.");
        } else {
            pending_ = request.request_id;
            std::snprintf(message_, sizeof(message_), "Saving...");
            read();
        }
        render();
        keys_ready_ = ready();
        UINavigator::instance().refreshSoftkeys();
    }
    void service() {
        const bool alive = inter_mcu_backend_link_alive();
        if (alive != alive_) {
            alive_ = alive;
            valid_ = false;
            if (!alive)
                std::snprintf(message_,
                              sizeof(message_),
                              "Disconnected. Save outcome is unconfirmed; it will not be retried "
                              "automatically.");
            if (alive)
                read();
        }
        SampleFileStatusMessage received;
        if (alive_ && inter_mcu_get_sample_file_status(&received) &&
            received.request_id == read_id_ && received.request_id != seen_ &&
            IsValidSampleFileStatus(received)) {
            seen_ = received.request_id;
            status_ = received;
            received_ = lv_tick_get();
            valid_ = true;
            if (pending_ && received.completed_request_id == pending_) {
                pending_ = 0;
                if (received.error == SAMPLE_FILE_OK)
                    std::snprintf(message_, sizeof(message_), "Saved: %s", received.path);
                else
                    std::snprintf(message_, sizeof(message_), "%s", errorText(received.error));
            } else if (received.busy && (!pending_ || received.active_request_id == pending_)) {
                std::snprintf(message_, sizeof(message_), "Saving... %u%%", received.progress);
            }
        }
        if (alive_ && uint32_t(lv_tick_get() - requested_) >= 300)
            read();
        render();
        const bool enabled = ready();
        if (enabled != keys_ready_) {
            keys_ready_ = enabled;
            UINavigator::instance().refreshSoftkeys();
        }
    }
    void render() {
        if (hint_ && std::strcmp(lv_label_get_text(hint_), message_))
            lv_label_set_text(hint_, message_);
    }
    uint16_t sample_id_;
    bool copy_, alive_ = false, valid_ = false, keys_ready_ = false;
    lv_obj_t* hint_ = nullptr;
    lv_obj_t* input_ = nullptr;
    lv_timer_t* timer_ = nullptr;
    uint32_t pending_ = 0, read_id_ = 0, requested_ = 0, received_ = 0, seen_ = 0;
    SampleFileStatusMessage status_;
    char message_[BROWSE_PATH_MAX + 80]{};
};
}  // namespace
std::shared_ptr<UIPage> createSampleFilePage(uint16_t sample_id, bool copy) {
    return std::make_shared<SampleFilePage>(sample_id, copy);
}
}  // namespace wavex_ui
