#include "ui/ui_pot_cal_page.h"

#include "debug/console_command.h"
#include "panel/pot_service.h"
#include "ui/ui_navigator.h"
#include "ui_theme.h"

#include <cstdio>
#include <cstring>
namespace wavex_ui {
namespace {
using Stage = WaveX::Panel::PotCalibrationSession::Stage;
void text(lv_obj_t* label, const char* value) {
    if (std::strcmp(lv_label_get_text(label), value))
        lv_label_set_text(label, value);
}
class PotCalPage : public UIPage {
   public:
    const char* name() const override { return "Pots"; }
    void onEnter(lv_obj_t* parent) override {
        root_ = lv_obj_create(parent);
        lv_obj_set_size(root_, lv_pct(100), lv_pct(100));
        lv_obj_set_style_bg_color(root_, UI_COLOR_BG, 0);
        lv_obj_set_style_border_width(root_, 0, 0);
        lv_obj_set_style_pad_all(root_, UI_PADDING_LARGE, 0);
        lv_obj_remove_flag(root_, LV_OBJ_FLAG_SCROLLABLE);
        auto label = [&](int y, const lv_font_t* font) {
            auto* obj = lv_label_create(root_);
            lv_obj_set_pos(obj, 0, y);
            lv_obj_set_width(obj, lv_pct(100));
            lv_obj_set_style_text_font(obj, font, 0);
            lv_obj_set_style_text_color(obj, UI_COLOR_FG, 0);
            return obj;
        };
        title_ = label(0, UI_FONT_TITLE);
        instruction_ = label(44, UI_FONT_BODY);
        for (int i = 0; i < 4; ++i)
            rows_[i] = label(130 + i * 46, UI_FONT_MONO_SMALL);
        result_ = label(330, UI_FONT_SMALL);
        note_ = label(378, UI_FONT_SMALL);
        lv_label_set_text(note_,
                          "Controls stay inactive during calibration. Save keeps the verified "
                          "range and direction.");
        refresh();
        timer_ = lv_timer_create(
            [](lv_timer_t* timer) {
                static_cast<PotCalPage*>(lv_timer_get_user_data(timer))->refresh();
            },
            50,
            this);
    }
    bool canLeave() override { return !wavex_panel::ReadPots().busy; }
    void onExit() override {
        wavex_panel::CancelPotCalibration();
        if (timer_)
            lv_timer_delete(timer_);
        timer_ = nullptr;
        if (root_)
            lv_obj_delete(root_);
        root_ = nullptr;
    }
    void onInput(const InputEvent& event) override {
        if (event.type == InputType::EncoderLeft || event.type == InputType::EncoderRight)
            select(event.steps() > 0 ? 1 : -1);
    }
    std::array<Softkey, NUM_SOFTKEYS> getSoftkeys() override {
        auto state = wavex_panel::ReadPots();
        const bool idle = state.stage == Stage::Idle;
        std::array<Softkey, NUM_SOFTKEYS> keys{};
        keys[0] = {"Back",
                   [] { UINavigator::instance().pop(); },
                   !state.busy,
                   "Finishing calibration operation"};
        keys[1] = {"Pot -",
                   [this] { select(-1); },
                   idle && !state.busy && selected_ > 0,
                   "Finish or cancel first"};
        keys[2] = {"Pot +",
                   [this] { select(1); },
                   idle && !state.busy && selected_ < 3,
                   "Finish or cancel first"};
        const char* action = idle                          ? "Start"
                             : state.stage == Stage::Range ? "Verify"
                             : state.stage == Stage::Ready ? "Save"
                                                           : "Turn clockwise";
        keys[3] = {action,
                   [this] { advance(); },
                   !state.busy && state.adc_ready && state.stage != Stage::Verify,
                   "Waiting for a valid sweep / ADC"};
        keys[4] = {"Cancel",
                   [] { wavex_panel::CancelPotCalibration(); },
                   !idle && !state.busy,
                   "No calibration in progress"};
        keys[5] = {"Disable",
                   [this] { wavex_panel::RequestPot(wavex_panel::PotCommand::Disable, selected_); },
                   idle && !state.busy && state.calibration[selected_].enabled,
                   "Pot is already inactive"};
        return keys;
    }
    size_t consoleState(char* out, size_t cap, size_t len) override {
        auto state = wavex_panel::ReadPots();
        using namespace WaveX::Debug;
        len = AppendKvInt(out, cap, len, "pot", selected_);
        len = AppendKvInt(out, cap, len, "calstage", static_cast<int>(state.stage));
        len = AppendKvInt(out, cap, len, "calprogress", state.progress);
        return AppendKvInt(out, cap, len, "calresult", state.last_result);
    }

   private:
    void select(int delta) {
        auto state = wavex_panel::ReadPots();
        if (state.busy || state.stage != Stage::Idle)
            return;
        selected_ = static_cast<uint8_t>(std::clamp(int(selected_) + delta, 0, 3));
        refresh();
        UINavigator::instance().refreshSoftkeys();
    }
    void advance() {
        const auto stage = wavex_panel::ReadPots().stage;
        auto command = stage == Stage::Idle    ? wavex_panel::PotCommand::Begin
                       : stage == Stage::Range ? wavex_panel::PotCommand::Verify
                       : stage == Stage::Ready ? wavex_panel::PotCommand::Save
                                               : wavex_panel::PotCommand::None;
        wavex_panel::RequestPot(command, selected_);
    }
    void refresh() {
        auto state = wavex_panel::ReadPots();
        char buffer[200];
        std::snprintf(buffer, sizeof(buffer), "Pot %u / RV112FF 20k", selected_ + 1);
        text(title_, buffer);
        const char* instruction =
            state.stage == Stage::Idle
                ? "Choose a pot and Start. Rotate it through two full turns, then choose Verify."
            : state.stage == Stage::Range
                ? "Capturing range: rotate through two full turns, then choose Verify."
            : state.stage == Stage::Verify ? "Range frozen. Make one complete CLOCKWISE turn to "
                                             "verify the shape and direction."
                                           : "Sweep verified. Save to enable this pot, or Cancel "
                                             "to keep its previous calibration.";
        text(instruction_, instruction);
        for (size_t i = 0; i < 4; ++i) {
            const auto& cal = state.calibration[i];
            std::snprintf(buffer,
                          sizeof(buffer),
                          "Pot %u: %s   A %u B %u   angle %u   %s",
                          static_cast<unsigned>(i + 1),
                          cal.enabled ? "calibrated" : "inactive",
                          state.wipers[i][0],
                          state.wipers[i][1],
                          state.readings[i].angle,
                          state.readings[i].valid ? "valid" : "no valid reading");
            text(rows_[i], buffer);
        }
        if (state.busy)
            std::snprintf(buffer, sizeof(buffer), "Working...");
        else if (state.last_result != ESP_OK)
            std::snprintf(
                buffer, sizeof(buffer), "Operation failed: %s", esp_err_to_name(state.last_result));
        else
            std::snprintf(buffer,
                          sizeof(buffer),
                          "ADC %s / scan errors %lu / verification %u%% / A %u..%u B %u..%u",
                          state.adc_ready ? "responding" : "unavailable",
                          static_cast<unsigned long>(state.errors),
                          state.progress,
                          state.candidate.a.low,
                          state.candidate.a.high,
                          state.candidate.b.low,
                          state.candidate.b.high);
        text(result_, buffer);
        unsigned flags = static_cast<unsigned>(state.stage) | (state.busy ? 8u : 0u) |
                         (state.adc_ready ? 16u : 0u) |
                         (state.calibration[selected_].enabled ? 32u : 0u);
        if (flags != soft_state_) {
            soft_state_ = flags;
            UINavigator::instance().refreshSoftkeys();
        }
    }
    uint8_t selected_ = 0;
    unsigned soft_state_ = ~0u;
    lv_timer_t* timer_ = nullptr;
    lv_obj_t *title_ = nullptr, *instruction_ = nullptr, *result_ = nullptr, *note_ = nullptr;
    std::array<lv_obj_t*, 4> rows_{};
};
}  // namespace
std::shared_ptr<UIPage> createPotCalPage() {
    return std::make_shared<PotCalPage>();
}
}  // namespace wavex_ui
