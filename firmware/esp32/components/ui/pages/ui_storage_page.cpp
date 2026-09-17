#include "../styles/ui_theme.h"
#include "esp_random.h"
#include "inter_mcu.h"
#include "ui/card_format_model.h"
#include "ui/ui_main_menu.h"
#include "ui/ui_navigator.h"
#include "ui/ui_page.h"

#include <cstring>

namespace wavex_ui {
using namespace WaveX::Protocol;
namespace {
uint32_t nextId() {
    static uint32_t id = esp_random();
    if (++id == 0)
        ++id;
    return id;
}
class UIStoragePage : public UIPage {
   public:
    const char* name() const override { return "Storage"; }
    void onEnter(lv_obj_t* parent) override {
        model_ = {};
        received_ = lv_tick_get();
        offline_ = true;
        root_ = lv_obj_create(parent);
        ui_theme_apply_container_style(root_, false);
        lv_obj_set_size(root_, lv_pct(100), lv_pct(100));
        lv_obj_set_flex_flow(root_, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_all(root_, UI_PADDING_LARGE, 0);
        lv_obj_set_style_pad_row(root_, UI_PADDING_LARGE, 0);
        auto* title = makeLabel("SD CARD", UI_FONT_TITLE);
        (void)title;
        makeLabel("Format Card erases all card data and creates the WaveX folders.", UI_FONT_BODY);
        makeLabel(
            "Back up your samples and saved work first. Playback stops during formatting. "
            "Keep the card inserted and power on until formatting finishes.",
            UI_FONT_BODY);
        status_ = makeLabel("Checking card...", UI_FONT_BODY);
        send(CARD_GET);
        timer_ = lv_timer_create(
            [](lv_timer_t* timer) {
                static_cast<UIStoragePage*>(lv_timer_get_user_data(timer))->poll();
            },
            100,
            this);
    }
    void onExit() override {
        if (timer_) {
            lv_timer_delete(timer_);
            timer_ = nullptr;
        }
        if (model_.Armed())
            inter_mcu_send_card_op(model_.Request(CARD_CANCEL, nextId()));
        status_ = nullptr;
        if (root_) {
            lv_obj_delete(root_);
            root_ = nullptr;
        }
    }
    std::array<Softkey, NUM_SOFTKEYS> getSoftkeys() override {
        std::array<Softkey, NUM_SOFTKEYS> keys{};
        keys[0] = {"Back", [] { UINavigator::instance().pop(); }};
        const bool busy = model_.Pending() || model_.State().state == CARD_FORMATTING;
        if (model_.Armed()) {
            keys[1] = {"Cancel", [this] { send(CARD_CANCEL); }};
            keys[5] = {"Erase all data",
                       [this] { send(CARD_CONFIRM_FORMAT); },
                       model_.CanConfirm() && !offline_,
                       "Waiting for confirmation from the card"};
        } else {
            keys[5] = {"Format Card",
                       [this] { send(CARD_PREPARE_FORMAT); },
                       !busy && !offline_,
                       "Waiting for the audio engine"};
        }
        return keys;
    }

   private:
    CardFormatModel model_;
    lv_obj_t* status_ = nullptr;
    lv_timer_t* timer_ = nullptr;
    uint32_t polled_ = 0, received_ = 0;
    bool offline_ = true;
    lv_obj_t* makeLabel(const char* text, const lv_font_t* font) {
        auto* object = lv_label_create(root_);
        ui_theme_apply_label_style(object, false);
        lv_obj_set_style_text_font(object, font, 0);
        lv_obj_set_width(object, lv_pct(100));
        lv_label_set_long_mode(object, LV_LABEL_LONG_WRAP);
        lv_label_set_text(object, text);
        return object;
    }
    void text(const char* value) {
        if (status_ && std::strcmp(lv_label_get_text(status_), value))
            lv_label_set_text(status_, value);
    }
    void send(uint8_t op) {
        const auto request = model_.Request(op, nextId());
        if (!IsValidCardOp(request))
            return;
        if (inter_mcu_send_card_op(request) != ESP_OK) {
            model_.SendFailed();
            text("Could not contact the audio engine. No format was retried.");
        } else if (op == CARD_PREPARE_FORMAT) {
            text("Preparing confirmation...");
        } else if (op == CARD_CONFIRM_FORMAT) {
            text("Format requested. Waiting for the result; do not remove the card.");
        } else if (op == CARD_CANCEL) {
            text("Formatting cancelled.");
        }
        polled_ = lv_tick_get();
        UINavigator::instance().refreshSoftkeys();
    }
    void poll() {
        CardStateMessage reply;
        const uint32_t now = lv_tick_get();
        if (inter_mcu_get_card_state(&reply) && model_.Accept(reply)) {
            received_ = now;
            offline_ = false;
            if (model_.UnknownResult()) {
                text(
                    "Format result is unknown after reconnection. Check the card before starting "
                    "again.");
            } else
                switch (reply.state) {
                    case CARD_CONFIRMATION:
                        text(model_.CanConfirm()
                                 ? "ALL CARD DATA WILL BE LOST. This cannot be undone. Choose "
                                   "Cancel or Erase all data."
                                 : "Format confirmation is not active on this page.");
                        break;
                    case CARD_FORMATTING:
                        text("Formatting card... Do not remove the card or power off.");
                        break;
                    case CARD_DONE:
                        text("Card formatted. WaveX folders are ready.");
                        break;
                    case CARD_FAILED:
                        text(reply.error == CARD_BUSY
                                 ? "A file operation is busy. Wait, then try again."
                             : reply.error == CARD_NOT_READY
                                 ? "No card is ready. Insert a card and try again."
                             : reply.error == CARD_BAD_CONFIRMATION
                                 ? "Confirmation expired or the card changed. Start again."
                             : reply.error == CARD_AUDIO_BUSY
                                 ? "Audio could not stop safely. Formatting was not started."
                                 : "Card preparation failed. The card may be erased or only partly "
                                   "initialized. Retry after checking it.");
                        break;
                    default:
                        text(reply.mounted ? "Card mounted. Format Card requires confirmation."
                                           : "Card not mounted. Insert a card to format it.");
                        break;
                }
            UINavigator::instance().refreshSoftkeys();
        }
        if (now - received_ > 5000u && !offline_) {
            offline_ = true;
            text(
                "Waiting for the audio engine. If formatting was confirmed, keep power on and the "
                "card inserted.");
            UINavigator::instance().refreshSoftkeys();
        }
        if (now - polled_ >= 1000u)
            send(CARD_GET);  // read-only recovery; never resend FORMAT
    }
};
}  // namespace
std::shared_ptr<UIPage> createStorageSettingsPage() {
    return std::make_shared<UIStoragePage>();
}
}  // namespace wavex_ui
