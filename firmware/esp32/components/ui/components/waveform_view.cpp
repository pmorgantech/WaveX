#include "waveform_view.h"

#include <algorithm>
#include <array>

namespace wavex_ui {

WaveformView::WaveformView(lv_obj_t* parent, lv_coord_t width, lv_coord_t height) {
    chart_ = lv_chart_create(parent);
    lv_obj_set_size(chart_, width, height);
    lv_chart_set_type(chart_, LV_CHART_TYPE_LINE);
    lv_chart_set_point_count(chart_, kPointCount);
    lv_chart_set_range(chart_, LV_CHART_AXIS_PRIMARY_Y, -32768, 32767);
    lv_chart_set_div_line_count(chart_, 4, 8);
    // Use circular; replace not available in this LVGL version
    lv_chart_set_update_mode(chart_, LV_CHART_UPDATE_MODE_CIRCULAR);
    series_ = lv_chart_add_series(
        chart_, lv_palette_main(LV_PALETTE_LIGHT_BLUE), LV_CHART_AXIS_PRIMARY_Y);
    clear();
}

void WaveformView::clear() {
    if (!chart_ || !series_)
        return;
    for (uint16_t i = 0; i < kPointCount; ++i) {
        lv_chart_set_value_by_id(chart_, series_, i, 0);
    }
    lv_chart_refresh(chart_);
}

void WaveformView::setSamples(const int16_t* samples, uint16_t count) {
    if (!samples || count == 0 || !chart_ || !series_) {
        clear();
        return;
    }

    // Find dynamic range for autoscaling
    int16_t min_v = samples[0];
    int16_t max_v = samples[0];
    for (uint16_t i = 1; i < count; ++i) {
        if (samples[i] < min_v)
            min_v = samples[i];
        if (samples[i] > max_v)
            max_v = samples[i];
    }
    // Expand range slightly to avoid flat lines when min==max
    int32_t pad = std::max<int32_t>(500, (max_v - min_v) / 8);
    int32_t y_min = std::max<int32_t>(-32768, (int32_t)min_v - pad);
    int32_t y_max = std::min<int32_t>(32767, (int32_t)max_v + pad);
    if (y_min == y_max) {
        y_min = std::max<int32_t>(-32768, y_min - 1);
        y_max = std::min<int32_t>(32767, y_max + 1);
    }
    lv_chart_set_range(chart_, LV_CHART_AXIS_PRIMARY_Y, y_min, y_max);

    // Down-sample to chart point count
    for (uint16_t i = 0; i < kPointCount; ++i) {
        uint32_t idx = static_cast<uint32_t>((static_cast<uint64_t>(i) * count) / kPointCount);
        if (idx >= count)
            idx = count - 1;
        lv_chart_set_value_by_id(chart_, series_, i, samples[idx]);
    }

    lv_chart_refresh(chart_);
}

}  // namespace wavex_ui
