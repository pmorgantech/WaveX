#pragma once

#include "envelope_sink.h"
#include "lvgl.h"
#include "spi_protocol/protocol.h"

#include <cstdint>
#include <vector>

namespace wavex_ui {

/**
 * @brief LVGL waveform renderer drawn as filled vertical spans.
 *
 * Draws a min/max envelope at ONE COLUMN PER PIXEL of its width. The
 * EnvelopeSink face is what an EnvelopePanel drives; the pages never call
 * setEnvelope() themselves any more.
 *
 * **Not an lv_chart.** It was one, and that cost 37-44 ms on the first frame
 * after entering Sample Edit or Sample Record - measured, see
 * docs/roadmap.md. An `lv_chart` of `LV_CHART_TYPE_LINE` with two series draws
 * ~1022 anti-aliased line segments, and anti-aliasing a waveform silhouette
 * buys nothing: the shape is a solid block of colour, not a curve anyone reads
 * the slope of. This draws one opaque fill per column from an
 * `LV_EVENT_DRAW_MAIN` handler instead - no anti-aliasing, no per-point widget
 * state, and adjacent identical columns merge into a single fill.
 *
 * **Why a column per pixel.** The previous version kept 512 columns whatever
 * its width and resampled whatever it was given onto them by nearest index.
 * That threw detail away twice: the cache served 1256 columns to the edit
 * page and the view kept 512 of them, and every column it did keep was one
 * picked column rather than the extremes of the 2.45 it stood for, so a
 * single-sample click could vanish from the display. Now the sink's column
 * count IS its pixel width, the panel asks the cache for exactly that many,
 * and nothing is resampled on the way to the screen. The resample paths
 * below stay only for a caller that hands over some other count; they are
 * exact (min/max merge) when reducing.
 *
 * Column storage is allocated once in the constructor and never resized
 * (`docs/esp32p4_coding_guide.md` §8).
 *
 * All methods are UI-task only, like every other LVGL call in this codebase.
 */
class WaveformView : public EnvelopeSink {
   public:
    /// @p width and @p height are PIXELS. A percentage or LV_SIZE_CONTENT
    /// width cannot name a column count, so it falls back to kFallbackColumns
    /// and draws resampled; give a pixel width and it will not.
    WaveformView(lv_obj_t* parent, int32_t width, int32_t height);
    ~WaveformView() override = default;

    lv_obj_t* root() const { return obj_; }

    // EnvelopeSink
    uint16_t columns() const override { return columns_; }

    /**
     * @brief Draws a min/max envelope (roadmap 1.5.5 item 2).
     *
     * `columns` holds `count * channels` values, channel-interleaved per
     * column. This does NOT auto-scale: an envelope's whole job is to show
     * level honestly, and rescaling a quiet passage to full height would say
     * the opposite of what it means.
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
     */
    void setEnvelope(const WaveX::Protocol::EnvelopeColumn* columns,
                     uint16_t count,
                     uint8_t channels) override;
    void clear() override;

   private:
    /// Used only when the constructor is given a width that is not a pixel
    /// count. One column per ~2.45 px of the design's 1256 px panel.
    static constexpr uint16_t kFallbackColumns = 512;

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
                  lv_draw_fill_dsc_t& dsc,
                  const lv_area_t& lane,
                  const lv_area_t& clip,
                  uint8_t channel) const;

    /// Maps a sample value onto a y pixel inside @p area.
    static int32_t valueToY(int32_t value, const lv_area_t& area);

    int16_t& colMin(uint8_t ch, uint16_t i) { return col_min_[ch * columns_ + i]; }
    int16_t& colMax(uint8_t ch, uint16_t i) { return col_max_[ch * columns_ + i]; }
    int16_t colMin(uint8_t ch, uint16_t i) const { return col_min_[ch * columns_ + i]; }
    int16_t colMax(uint8_t ch, uint16_t i) const { return col_max_[ch * columns_ + i]; }

    lv_obj_t* obj_ = nullptr;
    uint16_t columns_ = kFallbackColumns;

    /// kMaxChannels lanes of columns_ each, lane-major.
    std::vector<int16_t> col_min_;
    std::vector<int16_t> col_max_;

    /// 1 or 2. Drives the layout, so it is what decides whether the panel is
    /// showing you one trace or two.
    uint8_t channels_ = 1;

    bool has_data_ = false;
};

}  // namespace wavex_ui
