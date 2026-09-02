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
     * A stereo envelope draws as TWO STACKED TRACES - L above R, each in its
     * own half of the panel and each labelled - while a mono file uses the
     * full height (roadmap 1.5.7 item 1). Three properties this buys, and they
     * are the reason it is not one trace:
     *
     * - **A loop seam can be judged per channel.** A loop tuned to look clean
     *   on L can click audibly on R, and a combined trace cannot show that.
     * - **Nothing is hidden.** Summing was never an option here: an
     *   out-of-phase stereo sample sums to near silence and would draw a flat
     *   line for audio that is fine.
     * - **The channel layout is stated on screen.** The failure mode this
     *   whole item exists to avoid is the legacy preview path, which drew the
     *   LEFT CHANNEL ONLY and said nothing about it.
     *
     * This previously drew one trace spanning the extremes of both channels.
     * That was honest - it could only fail to separate L from R, never hide
     * anything - but it could not do the first or third of the above.
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

    /// L and R. The wire carries 1 or 2; anything beyond is clamped away
    /// rather than silently selecting which two channels you get.
    static constexpr uint8_t kMaxChannels = 2;

    // Grid, matching the div lines the chart used to draw. Rows are per LANE,
    // so each trace keeps a line on its own zero: 4 across a mono panel puts
    // one at the centre, and 2 within each half of a stereo panel does the
    // same for each channel. A single 4-row grid across both lanes would put
    // lines at neither zero.
    static constexpr uint8_t kGridRowsMono = 4;
    static constexpr uint8_t kGridRowsStereo = 2;
    static constexpr uint8_t kGridCols = 8;

    static void drawEventCb(lv_event_t* e);
    void drawSpans(lv_event_t* e) const;

    /// Content area of trace @p channel: the whole panel in mono, the top or
    /// bottom half in stereo, with the divider row excluded from both.
    lv_area_t laneArea(const lv_area_t& content, uint8_t channel) const;

    /// Draws one trace's filled spans into @p lane.
    void drawLane(lv_layer_t* layer,
                  lv_draw_rect_dsc_t& dsc,
                  const lv_area_t& lane,
                  uint8_t channel) const;

    /// Maps a sample value onto a y pixel inside @p area.
    int32_t valueToY(int32_t value, const lv_area_t& area) const;

    lv_obj_t* obj_ = nullptr;

    std::array<std::array<int16_t, kColumnCount>, kMaxChannels> col_min_{};
    std::array<std::array<int16_t, kColumnCount>, kMaxChannels> col_max_{};

    /// 1 or 2. Drives the layout, so it is what decides whether the panel is
    /// showing you one trace or two.
    uint8_t channels_ = 1;

    // Display range. Fixed full-scale for envelopes, autoscaled for samples.
    int32_t y_min_ = -32768;
    int32_t y_max_ = 32767;

    bool has_data_ = false;
};

}  // namespace wavex_ui
