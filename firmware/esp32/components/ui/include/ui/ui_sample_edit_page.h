#pragma once

#include "input_event.h"
#include "spi_protocol/protocol.h"
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
    enum Param : uint8_t {
        PARAM_START = 0,
        PARAM_END,
        PARAM_LOOP_START,
        PARAM_LOOP_END,
        PARAM_GAIN,
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

    // One envelope run in flight, staged into a buffer allocated once at its
    // ceiling and never resized: the UART task writes into it while the UI
    // task reads, so a reallocation mid-copy would be a use-after-free where
    // a torn value is merely one stale column.
    std::vector<WaveX::Protocol::EnvelopeColumn> run_columns_;
    std::vector<WaveX::Protocol::EnvelopeColumn> display_columns_;

    // Identity of the request in flight, read by the RX task to decide
    // whether a chunk still belongs to the view on screen. run_epoch_ is
    // bumped before each arming and re-checked after the copy, so a chunk
    // cannot be filed against a request that changed underneath it.
    volatile uint32_t run_epoch_ = 0;
    uint16_t pending_sample_id_ = 0;
    uint16_t pending_generation_ = 0;
    uint32_t pending_start_ = 0;
    uint32_t pending_end_ = 0;
    uint16_t pending_columns_ = 0;
    volatile uint16_t run_received_ = 0;
    volatile uint8_t run_channels_ = 0;
    volatile bool run_ready_ = false;
    bool request_in_flight_ = false;
    uint32_t request_sent_ms_ = 0;

    // Envelope chunks arrive on the UART RX task, which must not touch LVGL
    // (inter_mcu.h; ui-architecture.md) and must not touch the cache either,
    // since the cache allocates. The callback only fills the buffer and
    // raises these flags; ui_timer_ applies them on the UI task.
    lv_timer_t* ui_timer_ = nullptr;
    volatile bool waveform_dirty_ = false;
    volatile bool params_dirty_ = false;

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
