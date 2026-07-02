#pragma once

#include "lvgl.h"

#include <cstdint>
#include <vector>

namespace wavex_ui {

/**
 * @brief Lightweight LVGL waveform renderer using lv_chart.
 *
 * Accepts int16 samples (Q15 style) and renders a line plot scaled to the full
 * chart range. Designed for small buffers (<= a few thousand samples); data is
 * down-sampled to a fixed point count for display.
 */
class WaveformView {
   public:
    WaveformView(lv_obj_t* parent, lv_coord_t width, lv_coord_t height);

    lv_obj_t* root() const { return chart_; }

    /** Set and render new samples. */
    void setSamples(const int16_t* samples, uint16_t count);

    /** Clear the chart contents. */
    void clear();

   private:
    static constexpr uint16_t kPointCount = 256;

    lv_obj_t* chart_ = nullptr;
    lv_chart_series_t* series_ = nullptr;
};

}  // namespace wavex_ui
