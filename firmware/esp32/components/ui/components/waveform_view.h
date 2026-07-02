#pragma once

#include "lvgl.h"

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
    void clear();

   private:
    static constexpr uint16_t kPointCount = 256;

    lv_obj_t* chart_ = nullptr;
    lv_chart_series_t* series_ = nullptr;
};

}  // namespace wavex_ui
