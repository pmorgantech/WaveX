// WaveX CV Calibration Page (roadmap item 5 stage 5; analog-voice-board.md
// §3). Subclasses UISettingsPage for the list/edit mechanics; adds the
// calibration-specific behavior:
//  - every value change sends MSG_CV_CAL_SET (persist=0) so the analog
//    chain reflects the calibration live;
//  - a CV test mode (MSG_CV_TEST) drives steady CVs for corner-frequency
//    measurement ("Cut=0"/"Cut=1" softkeys) and VCA-silence verification;
//  - "Save" persists the table to the Daisy's SD card (persist=1);
//  - on entry the current table is fetched (MSG_CV_CAL_GET) and applied via
//    the deferred-update pattern (the CAL_RESP callback runs on the UART
//    task and must not touch LVGL; an lv_timer applies it in UI context).

#include "ui/ui_cv_cal_page.h"

#include <esp_log.h>

#include "config/hardware_config.h"
#include "inter_mcu.h"
#include "ui/ui_settings_page.h"

#include <atomic>
#include <cstring>

static const char* TAG = "UI_CV_CAL";

namespace wavex_ui {

namespace {

// Fixed-point UI model of one group's CvCal (gains/offsets x1000, k x100),
// plus the test-CV levels. Single instance: the page edits one group at a
// time (group row selects which; Stage A uses group 0).
struct CalModel {
    int group = 0;
    int cut_gain = 1000;  // x1000
    int cut_off = 0;      // x1000, signed
    int q_gain = 1000;
    int q_off = 0;
    int vca_gain = 1000;
    int vca_off = 0;
    int curve_k = 300;   // x100
    int test_cut = 50;   // percent
    int test_res = 0;    // percent
    int test_vca = 100;  // percent
};
CalModel s_model;
bool s_test_active = false;

// CAL_RESP handoff: written by the UART task, applied by the page's
// lv_timer in UI context.
WaveX::Protocol::CvCalMessage s_pending_cal;
std::atomic<bool> s_pending_flag{false};

WaveX::Protocol::CvCalMessage ModelToMessage(uint8_t persist) {
    return WaveX::Protocol::CvCalMessage(static_cast<uint8_t>(s_model.group),
                                         persist,
                                         static_cast<float>(s_model.cut_gain) / 1000.0f,
                                         static_cast<float>(s_model.cut_off) / 1000.0f,
                                         static_cast<float>(s_model.q_gain) / 1000.0f,
                                         static_cast<float>(s_model.q_off) / 1000.0f,
                                         static_cast<float>(s_model.vca_gain) / 1000.0f,
                                         static_cast<float>(s_model.vca_off) / 1000.0f,
                                         static_cast<float>(s_model.curve_k) / 100.0f);
}

void SendCal(uint8_t persist) {
    if (inter_mcu_send_cv_cal_set(ModelToMessage(persist)) != ESP_OK) {
        ESP_LOGW(TAG, "CV cal send failed");
    }
}

void SendTest() {
    WaveX::Protocol::CvTestMessage test(static_cast<uint8_t>(s_model.group),
                                        s_test_active ? 1 : 0,
                                        static_cast<float>(s_model.test_cut) / 100.0f,
                                        static_cast<float>(s_model.test_res) / 100.0f,
                                        static_cast<float>(s_model.test_vca) / 100.0f);
    if (inter_mcu_send_cv_test(test) != ESP_OK) {
        ESP_LOGW(TAG, "CV test send failed");
    }
}

void OnCalResp(const WaveX::Protocol::CvCalMessage& cal, void* /*user_data*/) {
    // UART-task context: stage only, no LVGL (ui-architecture.md).
    s_pending_cal = cal;
    s_pending_flag.store(true, std::memory_order_release);
}

class UICvCalPage : public UISettingsPage {
   public:
    UICvCalPage() : UISettingsPage("CV Calibration") {
        // Order matters: indices are used by applyPendingCal().
        addSetting("Group", s_model.group, 0, WAVEX_ANALOG_CV_GROUPS_MAX - 1, [](int v) {
            s_model.group = v;
            inter_mcu_send_cv_cal_get(static_cast<uint8_t>(v));  // fetch that group's cal
        });
        addSetting("Cut gain x1000", s_model.cut_gain, 0, 2000, [](int v) {
            s_model.cut_gain = v;
            SendCal(0);
        });
        addSetting("Cut offs x1000", s_model.cut_off, -500, 500, [](int v) {
            s_model.cut_off = v;
            SendCal(0);
        });
        addSetting("Res gain x1000", s_model.q_gain, 0, 2000, [](int v) {
            s_model.q_gain = v;
            SendCal(0);
        });
        addSetting("Res offs x1000", s_model.q_off, -500, 500, [](int v) {
            s_model.q_off = v;
            SendCal(0);
        });
        addSetting("VCA gain x1000", s_model.vca_gain, 0, 2000, [](int v) {
            s_model.vca_gain = v;
            SendCal(0);
        });
        addSetting("VCA offs x1000", s_model.vca_off, -500, 500, [](int v) {
            s_model.vca_off = v;
            SendCal(0);
        });
        addSetting("Curve k x100", s_model.curve_k, 0, 600, [](int v) {
            s_model.curve_k = v;
            SendCal(0);
        });
        addSetting("Test cutoff %", s_model.test_cut, 0, 100, [](int v) {
            s_model.test_cut = v;
            if (s_test_active)
                SendTest();
        });
        addSetting("Test res %", s_model.test_res, 0, 100, [](int v) {
            s_model.test_res = v;
            if (s_test_active)
                SendTest();
        });
        addSetting("Test VCA %", s_model.test_vca, 0, 100, [](int v) {
            s_model.test_vca = v;
            if (s_test_active)
                SendTest();
        });
    }

    const char* name() const override { return "CV Calibration"; }

    void onEnter(lv_obj_t* parent) override {
        UISettingsPage::onEnter(parent);
        inter_mcu_set_cv_cal_listener(OnCalResp, nullptr);
        inter_mcu_send_cv_cal_get(static_cast<uint8_t>(s_model.group));
        // Applies a pending CAL_RESP in UI/LVGL context (onEnter runs under
        // the LVGL lock, so creating the timer here is safe).
        timer_ = lv_timer_create(
            [](lv_timer_t* t) {
                auto* page = static_cast<UICvCalPage*>(lv_timer_get_user_data(t));
                page->applyPendingCal();
            },
            100,
            this);
    }

    void onExit() override {
        if (timer_) {
            lv_timer_del(timer_);
            timer_ = nullptr;
        }
        inter_mcu_set_cv_cal_listener(nullptr, nullptr);
        // Never leave a steady test CV driving the analog chain.
        if (s_test_active) {
            s_test_active = false;
            SendTest();
        }
        UISettingsPage::onExit();
    }

    std::array<Softkey, NUM_SOFTKEYS> getSoftkeys() override {
        std::array<Softkey, NUM_SOFTKEYS> keys{};
        keys[0] = {"Back", []() { UINavigator::instance().pop(); }};
        keys[1] = {editingValue_ ? "Done" : "Edit", [this]() { toggleEditMode(); }};
        keys[2] = {s_test_active ? "Test:ON" : "Test:off", []() {
                       s_test_active = !s_test_active;
                       SendTest();
                   }};
        // Corner-measurement presets (§3 procedure: measure the filter
        // corner at cutoff = 0 and 1 with the VCA fully open).
        keys[3] = {"Cut=0", [this]() { setTestCutoff(0); }};
        keys[4] = {"Cut=1", [this]() { setTestCutoff(100); }};
        keys[5] = {"Save", []() { SendCal(1); }};
        return keys;
    }

   private:
    lv_timer_t* timer_ = nullptr;

    void setTestCutoff(int percent) {
        s_model.test_cut = percent;
        s_model.test_vca = 100;
        s_test_active = true;
        SendTest();
        syncModelToRows();
    }

    void applyPendingCal() {
        if (!s_pending_flag.exchange(false, std::memory_order_acquire)) {
            return;
        }
        const auto& c = s_pending_cal;
        if (c.group != s_model.group) {
            return;  // stale response for a previously-selected group
        }
        s_model.cut_gain = static_cast<int>(c.vcf_cut_gain * 1000.0f);
        s_model.cut_off = static_cast<int>(c.vcf_cut_off * 1000.0f);
        s_model.q_gain = static_cast<int>(c.vcf_q_gain * 1000.0f);
        s_model.q_off = static_cast<int>(c.vcf_q_off * 1000.0f);
        s_model.vca_gain = static_cast<int>(c.vca_gain * 1000.0f);
        s_model.vca_off = static_cast<int>(c.vca_off * 1000.0f);
        s_model.curve_k = static_cast<int>(c.cutoff_k * 100.0f);
        syncModelToRows();
        ESP_LOGI(TAG, "Calibration for group %u loaded", (unsigned)c.group);
    }

    // Pushes s_model back into the settings rows (indices match the
    // constructor's addSetting order) and redraws.
    void syncModelToRows() {
        const int values[] = {s_model.group,
                              s_model.cut_gain,
                              s_model.cut_off,
                              s_model.q_gain,
                              s_model.q_off,
                              s_model.vca_gain,
                              s_model.vca_off,
                              s_model.curve_k,
                              s_model.test_cut,
                              s_model.test_res,
                              s_model.test_vca};
        for (size_t i = 0; i < settings_.size() && i < sizeof(values) / sizeof(values[0]); ++i) {
            settings_[i].value = values[i];
        }
        rebuildList();
    }
};

}  // namespace

std::shared_ptr<UIPage> createCvCalPage() {
    return std::make_shared<UICvCalPage>();
}

}  // namespace wavex_ui
