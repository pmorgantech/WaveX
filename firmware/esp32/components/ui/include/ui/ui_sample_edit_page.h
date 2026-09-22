#pragma once

#include "components/ui_value_tile.h"
#include "envelope_panel.h"
#include "input_event.h"
#include "spi_protocol/protocol.h"
#include "ui_page.h"

#include <array>
#include <atomic>
#include <memory>

namespace wavex_ui {

/**
 * @brief Sample edit / region view.
 *
 * Shows the loaded sample's waveform with a start/end region and a zoom
 * window, laid out per the WaveX Wireframes v2 edit screen.
 *
 * Scope note: START, END, ZOOM, GAIN, LOOP and the fades are all real - they
 * are sent to the backend via MSG_SAMPLE_EDIT_SET, so moving them changes what you
 * hear. (This comment previously said GAIN and LOOP were "drawn but inert",
 * which stopped being true when sendEdit() started carrying them.) Shifted Save/Save As open
 * standalone sidecar/copy jobs. Normalize remains part of the future offline render pipeline.
 */
class UISampleEditPage : public UIPage {
   public:
    UISampleEditPage() = default;

    const char* name() const override { return "SampleEdit"; }

    void onEnter(lv_obj_t* parent) override;
    void onExit() override;
    size_t consoleState(char* out, size_t size, size_t len) override;
    bool consoleCommand(const char* args, char* out, size_t size) override;
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
        PARAM_CROSSFADE,
        PARAM_CHANNEL,
        PARAM_COUNT
    };
    // Four cards fit the strip; the fifth (GAIN) shares the last slot and is
    // reached by paging, so the row stays at the design's 305px pitch.
    static constexpr int kVisibleCards = 4;

    lv_obj_t* root_ = nullptr;
    lv_obj_t* status_label_ = nullptr;
    lv_obj_t* info_label_ = nullptr;
    lv_obj_t* marker_s_ = nullptr;
    lv_obj_t* marker_e_ = nullptr;
    ValueTile cards_[PARAM_COUNT] = {};
    lv_obj_t* marker_ls_ = nullptr;
    lv_obj_t* marker_le_ = nullptr;

    lv_obj_t* wave_panel_ = nullptr;
    std::unique_ptr<class WaveformView> waveform_;

    // Loop splice view (roadmap 1.5.6 item 1): the audio just BEFORE loop_end
    // on the left and just AFTER loop_start on the right, butted at a centre
    // seam. This is unprocessed PCM; measured playback jumps account for crossfade. Two
    // ordinary WaveformViews rather than a mode inside one: each half is a
    // normal envelope render, so stacked L/R and everything else it already
    // does come along unchanged, and the widget's tests keep covering it.
    std::unique_ptr<class WaveformView> splice_left_;
    std::unique_ptr<class WaveformView> splice_right_;
    lv_obj_t* splice_seam_ = nullptr;
    lv_obj_t* splice_label_ = nullptr;
    /// What the panel is currently showing, so a mode change can be detected
    /// and turned into a redraw plus a fetch of the two new windows.
    bool splice_shown_ = false;

    bool has_sample_ = false;

    // The waveform cycle - requests, chunk assembly, the cache, rendering -
    // for all three views. Shared with the sample browser and the record
    // page; see envelope_panel.h. What this page keeps is what it last told
    // the panel, so syncWindows() can tell a new sample from a tick.
    EnvelopePanel panel_;
    uint16_t panel_sample_id_ = 0;
    uint16_t panel_generation_ = 0;
    uint32_t panel_total_frames_ = 0;
    /// The status line is showing a waveform problem, to be replaced by the
    /// sample name once the trace is drawn.
    bool wave_status_shown_ = false;

    // Redraw request, applied by ui_timer_ on the UI task.
    //
    // Raised only from UI-task code. Atomic defensively rather than by
    // necessity, so that a future comm-side caller is correct by default; do
    // not read this as evidence that the RX task touches it today.
    lv_timer_t* ui_timer_ = nullptr;
    std::atomic<bool> params_dirty_{false};

    // Touch-drag coalescing for the marker handles. LVGL fires PRESSING at the
    // display refresh rate, so sending an edit per event would put ~30
    // MSG_SAMPLE_EDIT_SET on the link per second of drag. Non-zero means one
    // is owed; serviceUi() sends it and clears this.
    uint32_t edit_due_ms_ = 0;
    uint8_t marker_drag_mask_ = 0;

    // Sample geometry, from the backend's cached SampleMetadata for the
    // current sample (see ui/current_sample.h).
    uint32_t total_frames_ = 0;
    uint32_t sample_rate_ = 48000;
    // Waveform revision alongside the Pool id. A channel-mapping edit or PCM
    // replacement invalidates cached envelopes; marker-only edits do not.
    uint16_t generation_ = 0;

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

    void buildWaveformPanel(lv_obj_t* parent);
    void buildParamStrip(lv_obj_t* parent);
    bool layoutParamStrip();
    void buildParamCard(uint8_t parameter);
    int first_card_ = 0;
    void buildInfoStrip(lv_obj_t* parent);

    static void uiTimerCb(lv_timer_t* t);
    void serviceUi();

    void toggleAudition();
    void adjustFocused(int steps);
    void setZoom(int direction);
    void zoomToFit();
    void clampMarkers();
    void sendEdit();
    void requestSeam(bool snap);
    uint32_t seam_pending_ = 0, seam_deadline_ = 0, edit_wait_until_ = 0;
    uint8_t crossfade_ms_ = 0;
    uint8_t channel_mode_ = WaveX::Protocol::SAMPLE_CH_AS_RECORDED, source_channels_ = 1;
    WaveX::Protocol::SampleEditMessage sent_edit_;
    char seam_text_[160]{};
    WaveX::Protocol::SampleSeamStatus seam_result_;
    void applyMeta(const WaveX::Protocol::SampleMetadata& m);
    void refreshParams();
    void refreshFocusRing();
    /// Tells the panel the current sample and every view's window.
    void syncWindows();

    /// True while the panel should show the loop seam rather than the region.
    bool spliceActive() const;
    /// Frames either side of the seam. Equal on both halves so the two can be
    /// compared by eye; 0 when there is nothing sensible to show.
    uint32_t spliceHalfSpan() const;
    /// Swaps the panel between the continuous view and the splice pair.
    void updateWaveformMode();

    /// Touch handling for the four region/loop handles.
    static void handleEventCb(lv_event_t* e);
    void onHandleDrag(lv_event_t* e, lv_obj_t* target);
    /// Frame under the pointer, mapped through the current zoom window.
    /// False when there is no pointer or the geometry is not ready.
    bool pointerFrame(lv_event_t* e, uint32_t& out_frame) const;
    uint16_t currentSampleId() const;
    void refreshStatus(const char* text);
};

std::shared_ptr<UIPage> createSampleEditPage();

}  // namespace wavex_ui
