#pragma once

#include "input_event.h"
#include "ui_page.h"

#include <array>
#include <memory>
#include <vector>

namespace wavex_ui {

/**
 * @brief Sample edit / region view.
 *
 * Shows the loaded sample's waveform with a start/end region and a zoom
 * window, laid out per the WaveX Wireframes v2 edit screen.
 *
 * Scope note: START, END and ZOOM are real - they set the range the backend
 * is asked to render, so moving them changes what you see. GAIN, LOOP,
 * Normalize and Save are drawn but inert: nothing in the protocol carries a
 * per-sample gain, loop point, or a write-back, so they are marked as such
 * rather than wired to controls that would silently do nothing.
 */
class UISampleEditPage : public UIPage {
   public:
    UISampleEditPage() = default;

    const char* name() const override { return "SampleEdit"; }

    void onEnter(lv_obj_t* parent) override;
    void onExit() override;
    void onInput(const InputEvent& evt) override;
    std::array<Softkey, NUM_SOFTKEYS> getSoftkeys() override;
    std::array<Softkey, NUM_SOFTKEYS> getShiftedSoftkeys() override;

   private:
    // Encoder-focusable parameters, in the order the strip shows them.
    enum Param : uint8_t { PARAM_START = 0, PARAM_END, PARAM_COUNT };

    struct ParamCard {
        lv_obj_t* card;
        lv_obj_t* value;
        lv_obj_t* bar;   // fill; nullptr for the inert cards
        lv_obj_t* knob;  // slider handle
    };

    lv_obj_t* root_ = nullptr;
    lv_obj_t* status_label_ = nullptr;
    lv_obj_t* info_label_ = nullptr;
    lv_obj_t* marker_s_ = nullptr;
    lv_obj_t* marker_e_ = nullptr;
    ParamCard cards_[PARAM_COUNT] = {};

    std::unique_ptr<class WaveformView> waveform_;
    bool has_sample_ = false;
    std::vector<int16_t> preview_buffer_;
    uint32_t expected_len_ = 0;

    // Wave chunks arrive on the UART RX task, which must not touch LVGL
    // (inter_mcu.h; ui-architecture.md). The callback only fills the buffer
    // and raises these flags; ui_timer_ applies them on the UI task.
    lv_timer_t* ui_timer_ = nullptr;
    volatile bool waveform_dirty_ = false;
    volatile bool params_dirty_ = false;

    // Encoder movement coalescing. One detent used to fire a preview request
    // of its own, so a single turn queued a burst of them.
    uint32_t request_due_ms_ = 0;

    // Region, in frames, within the previewable window.
    uint32_t start_frame_ = 0;
    uint32_t end_frame_ = 0;
    uint32_t window_frames_ = 0;  // zoom: how much of the sample the view spans
    uint8_t focus_ = PARAM_START;
    bool auditioning_ = false;

    static void waveChunkStatic(uint32_t offset,
                                const int16_t* samples,
                                uint16_t count,
                                void* user);
    void handleWaveChunk(uint32_t offset, const int16_t* samples, uint16_t count);

    void buildWaveformPanel(lv_obj_t* parent);
    void buildParamStrip(lv_obj_t* parent);
    void buildInfoStrip(lv_obj_t* parent);

    static void uiTimerCb(lv_timer_t* t);
    void serviceUi();

    void toggleAudition();
    void adjustFocused(int steps);
    void setZoom(int direction);
    void refreshParams();
    void refreshFocusRing();
    void requestWaveform();
    void refreshStatus(const char* text);
};

std::shared_ptr<UIPage> createSampleEditPage();

}  // namespace wavex_ui
