#pragma once

#include "lvgl.h"
#include "spi_protocol/protocol.h"

#include <cstdint>

namespace wavex_ui {

/**
 * @brief LVGL waveform renderer built on lv_chart.
 *
 * Accepts int16 samples and down-samples to a fixed number of points for
 * display. Intended for lightweight preview rendering.
 */
class WaveformView {
   public:
    WaveformView(lv_obj_t* parent, lv_coord_t width, lv_coord_t height);

    lv_obj_t* root() const { return chart_; }

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
    // Matches the design's 1256 px waveform panel closely enough that a point
    // is about two pixels, while keeping the per-redraw line count - two
    // series - well under what the old 256-point chart made it look like the
    // budget was.
    static constexpr uint16_t kPointCount = 512;

    lv_obj_t* chart_ = nullptr;
    lv_chart_series_t* series_ = nullptr;      // sample trace, or envelope max
    lv_chart_series_t* min_series_ = nullptr;  // envelope min; unused by setSamples
};

}  // namespace wavex_ui
