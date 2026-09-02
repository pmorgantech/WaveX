#pragma once

#include "envelope_fetcher.h"
#include "input_event.h"
#include "spi_protocol/protocol.h"
#include "ui_page.h"

#include <array>
#include <atomic>
#include <memory>
#include <vector>

namespace wavex_ui {

/**
 * @brief Sample edit / region view.
 *
 * Shows the loaded sample's waveform with a start/end region and a zoom
 * window, laid out per the WaveX Wireframes v2 edit screen.
 *
 * Scope note: START, END, ZOOM, GAIN, LOOP and the fades are all real - they
 * are sent to the backend via MSG_SAMPLE_EDIT, so moving them changes what you
 * hear. (This comment previously said GAIN and LOOP were "drawn but inert",
 * which stopped being true when sendEdit() started carrying them.) Normalize
 * and Save remain unwired: nothing in the protocol carries a write-back.
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
    enum Param : uint8_t {
        PARAM_START = 0,
        PARAM_END,
        PARAM_LOOP_START,
        PARAM_LOOP_END,
        PARAM_GAIN,
        PARAM_FADE_IN,
        PARAM_FADE_OUT,
        PARAM_COUNT
    };
    // Four cards fit the strip; the fifth (GAIN) shares the last slot and is
    // reached by paging, so the row stays at the design's 305px pitch.
    static constexpr int kVisibleCards = 4;

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
    lv_obj_t* marker_ls_ = nullptr;
    lv_obj_t* marker_le_ = nullptr;

    std::unique_ptr<class WaveformView> waveform_;
    bool has_sample_ = false;

    // The envelope run in flight, including the staging buffer, the
    // cross-task chunk assembly and the timeout/retry budget. Shared with the
    // sample browser's detail panel; see envelope_fetcher.h for why the
    // release/acquire handling lives there rather than being written twice.
    EnvelopeFetcher fetcher_;

    // Where render() merges cached tier columns for this view. Stays here
    // rather than in the fetcher: it is sized by what this page displays, not
    // by what one run can carry.
    std::vector<WaveX::Protocol::EnvelopeColumn> display_columns_;

    // Redraw requests, applied by ui_timer_ on the UI task.
    //
    // Unlike the fetcher's own state, these are raised only from UI-task code
    // - the envelope callback signals completion inside the fetcher, not
    // through these. They are atomic defensively rather than by necessity, so that a
    // future comm-side caller is correct by default; do not read this as
    // evidence that the RX task touches them today, and do not add one without
    // reading the ordering note above first.
    lv_timer_t* ui_timer_ = nullptr;
    std::atomic<bool> waveform_dirty_{false};
    std::atomic<bool> params_dirty_{false};

    // Encoder movement coalescing. One detent used to fire an envelope request
    // of its own, so a single turn queued a burst of them.
    uint32_t request_due_ms_ = 0;

    // Sample geometry, from the browse listing via SampleBrowserState.
    uint32_t total_frames_ = 0;
    uint32_t sample_rate_ = 48000;

    // Markers, in frames, absolute within the sample.
    uint32_t start_frame_ = 0;
    uint32_t end_frame_ = 0;
    uint32_t loop_start_ = 0;
    uint32_t loop_end_ = 0;
    bool loop_enabled_ = false;
    int16_t gain_db_x10_ = 0;  // -240..+120, i.e. -24.0 to +12.0 dB
    // Region fades, milliseconds. Default to the de-click length rather than
    // 0: a region that starts mid-waveform starts on a step, so de-clicking is
    // the default state and turning it off is the deliberate act.
    uint16_t fade_in_ms_ = WaveX::Protocol::kDefaultDeclickMs;
    uint16_t fade_out_ms_ = WaveX::Protocol::kDefaultDeclickMs;

    // Zoom: the visible span, and where it starts. Zooming keeps the focused
    // marker in view rather than always anchoring at zero, or zooming in far
    // enough to be useful would push the thing you are adjusting off-screen.
    uint32_t view_start_ = 0;
    uint32_t view_frames_ = 0;
    uint8_t focus_ = PARAM_START;
    bool auditioning_ = false;

    static void envelopeChunkStatic(const WaveX::Protocol::EnvelopeChunkMessage& header,
                                    const WaveX::Protocol::EnvelopeColumn* columns,
                                    void* user);
    void handleEnvelopeChunk(const WaveX::Protocol::EnvelopeChunkMessage& header,
                             const WaveX::Protocol::EnvelopeColumn* columns);

    void buildWaveformPanel(lv_obj_t* parent);
    void buildParamStrip(lv_obj_t* parent);
    void layoutParamStrip();
    void buildInfoStrip(lv_obj_t* parent);

    static void uiTimerCb(lv_timer_t* t);
    void serviceUi();

    void toggleAudition();
    void adjustFocused(int steps);
    void setZoom(int direction);
    void zoomToFit();
    void clampMarkers();
    void sendEdit();
    void applyMeta(const WaveX::Protocol::SampleMetadata& m);
    void refreshParams();
    void refreshFocusRing();
    void requestWaveform();
    void drawWaveform();  ///< UI task only: cache -> WaveformView.
    uint16_t currentSampleId() const;
    uint16_t currentGeneration() const;
    void refreshStatus(const char* text);
};

std::shared_ptr<UIPage> createSampleEditPage();

}  // namespace wavex_ui
