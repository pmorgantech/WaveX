#pragma once

#include "../components/envelope_panel.h"
#include "input_event.h"
#include "ui_page.h"

#include <array>
#include <cstdint>
#include <memory>

namespace wavex_ui {

/**
 * @brief The Record tab of the Sample group.
 *
 * Recording is not implemented: the backend acknowledges SAMPLE_CTRL and does
 * nothing with it (audio_engine OnSampleCtrl, "review C2"), and roadmap Phase
 * 1 rebuilds the capture path. Until then this page shows the current sample's
 * whole-file envelope through the same EnvelopePanel the Browse and Edit tabs
 * use, and says plainly that the Record key is inert. It used to drive its own
 * decimated-preview protocol (MSG_PREVIEW_REQ / MSG_WAVE_CHUNK) and report
 * "Recording..." on a command the backend ignores; both are gone.
 */
class UISampleRecordPage : public UIPage {
   public:
    UISampleRecordPage() = default;

    const char* name() const override { return "SampleRecord"; }

    void onEnter(lv_obj_t* parent) override;
    void onExit() override;
    std::array<Softkey, NUM_SOFTKEYS> getSoftkeys() override;

   private:
    lv_obj_t* root_ = nullptr;
    lv_obj_t* name_label_ = nullptr;
    lv_obj_t* status_label_ = nullptr;
    lv_timer_t* ui_timer_ = nullptr;
    std::unique_ptr<class WaveformView> waveform_;

    // The panel owns the chunk listener and the request cycle; this page only
    // tells it which sample to show and words the outcome.
    EnvelopePanel panel_;
    // What the panel was last told, so a reload (new generation) or a Load on
    // another tab is noticed without re-sending an unchanged sample.
    uint16_t shown_sample_id_ = 0;
    uint16_t shown_generation_ = 0;
    bool no_sample_shown_ = false;    // the "No sample selected" text is up
    bool wave_status_shown_ = false;  // the status line is about the request

    static void uiTimerCb(lv_timer_t* timer);
    void serviceUi();
    void syncSample();
    void setStatus(const char* text);
};

std::shared_ptr<UIPage> createSampleRecordPage();

}  // namespace wavex_ui
