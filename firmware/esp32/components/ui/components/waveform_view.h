#pragma once

#include "lvgl.h"
#include "spi_protocol/protocol.h"

#include <array>
#include <cstdint>

namespace wavex_ui {

/**
 * @brief LVGL waveform renderer drawn as filled vertical spans.
 *
 * Accepts int16 samples and down-samples to a fixed number of columns for
 * display. Intended for lightweight preview rendering.
 *
 * **Not an lv_chart.** It was one, and that cost 37-44 ms on the first frame
 * after entering Sample Edit or Sample Record - measured, see
 * docs/backlog.md. An `lv_chart` of `LV_CHART_TYPE_LINE` with two series draws
 * ~1022 anti-aliased line segments, and anti-aliasing a waveform silhouette
 * buys nothing: the shape is a solid block of colour, not a curve anyone reads
 * the slope of. This draws one opaque rectangle per column from an
 * `LV_EVENT_DRAW_MAIN` handler instead - no anti-aliasing, no per-point widget
 * state, and adjacent identical columns merge into a single rectangle.
 *
 * Column data lives in fixed `std::array` members, so nothing here allocates
 * after construction (`docs/esp32p4_coding_guide.md` §8).
 *
 * All methods are UI-task only, like every other LVGL call in this codebase.
 */
class WaveformView {
   public:
    WaveformView(lv_obj_t* parent, lv_coord_t width, lv_coord_t height);

    lv_obj_t* root() const { return obj_; }

    void setSamples(const int16_t* samples, uint16_t count);

    /**
     * @brief Draws a min/max envelope (roadmap 1.5.5 item 2).
     *
     * `columns` holds `count * channels` values, channel-interleaved per
     * column. Unlike setSamples() this does NOT auto-scale: an envelope's
     * whole job is to show level honestly, and rescaling a quiet passage to
     * full height would say the opposite of what it means.
     *
     * A stereo envelope is drawn as one trace spanning the extremes of BOTH
     * channels - not summed. Summing an out-of-phase stereo sample draws a
     * flat line for audio that is fine; taking the extremes cannot hide
     * anything, it can only fail to separate L from R. Two stacked traces is
     * roadmap 1.5.7, and it needs the layout decision made there.
     */
    void setEnvelope(const WaveX::Protocol::EnvelopeColumn* columns,
                     uint16_t count,
                     uint8_t channels);
    void clear();

   private:
    // One column per ~2.45 px of the design's 1256 px panel. Kept at the
    // chart-era value: with spans rather than a polyline the cost is no longer
    // dominated by column count, so there is no reason to show less detail.
    static constexpr uint16_t kColumnCount = 512;

    // Grid, matching the div lines the chart used to draw.
    static constexpr uint8_t kGridRows = 4;
    static constexpr uint8_t kGridCols = 8;

    static void drawEventCb(lv_event_t* e);
    void drawSpans(lv_event_t* e) const;

    /// Maps a sample value onto a y pixel inside @p area.
    int32_t valueToY(int32_t value, const lv_area_t& area) const;

    lv_obj_t* obj_ = nullptr;

    std::array<int16_t, kColumnCount> col_min_{};
    std::array<int16_t, kColumnCount> col_max_{};

    // Display range. Fixed full-scale for envelopes, autoscaled for samples.
    int32_t y_min_ = -32768;
    int32_t y_max_ = 32767;

    bool has_data_ = false;
};

}  // namespace wavex_ui
