#include "waveform_view.h"

#include <algorithm>

namespace wavex_ui {
namespace {

// The trace colour the chart used, kept so the look does not change with the
// drawing method.
constexpr uint32_t kTraceColor = 0x29B6F6;  // LV_PALETTE_LIGHT_BLUE main
constexpr uint32_t kGridColor = 0x2A2A2A;

}  // namespace

WaveformView::WaveformView(lv_obj_t* parent, lv_coord_t width, lv_coord_t height) {
    obj_ = lv_obj_create(parent);
    lv_obj_set_size(obj_, width, height);

    // Transparent: the owning panel already paints the background and border,
    // and a second opaque fill underneath the spans is a full-panel draw task
    // for nothing.
    lv_obj_set_style_bg_opa(obj_, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(obj_, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(obj_, 0, LV_PART_MAIN);
    lv_obj_remove_flag(obj_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(obj_, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_add_event_cb(obj_, drawEventCb, LV_EVENT_DRAW_MAIN, this);
    clear();
}

void WaveformView::clear() {
    col_min_.fill(0);
    col_max_.fill(0);
    y_min_ = -32768;
    y_max_ = 32767;
    has_data_ = false;
    if (obj_) {
        lv_obj_invalidate(obj_);
    }
}

void WaveformView::setEnvelope(const WaveX::Protocol::EnvelopeColumn* columns,
                               uint16_t count,
                               uint8_t channels) {
    if (!columns || count == 0 || channels == 0 || !obj_) {
        clear();
        return;
    }

    // Fixed full-scale range. See the header: an envelope that rescales itself
    // cannot be read as a level, which is most of what it is for.
    y_min_ = -32768;
    y_max_ = 32767;

    for (uint16_t i = 0; i < kColumnCount; ++i) {
        uint32_t idx = static_cast<uint32_t>((static_cast<uint64_t>(i) * count) / kColumnCount);
        if (idx >= count) {
            idx = count - 1;
        }

        // Widest excursion across the channels in this column - never an
        // average, which is what would let phase cancellation draw silence.
        int32_t lo = 32767;
        int32_t hi = -32768;
        for (uint8_t ch = 0; ch < channels; ++ch) {
            const WaveX::Protocol::EnvelopeColumn& c = columns[idx * channels + ch];
            lo = std::min<int32_t>(lo, c.min_sample);
            hi = std::max<int32_t>(hi, c.max_sample);
        }

        col_min_[i] = static_cast<int16_t>(lo);
        col_max_[i] = static_cast<int16_t>(hi);
    }

    has_data_ = true;
    lv_obj_invalidate(obj_);
}

void WaveformView::setSamples(const int16_t* samples, uint16_t count) {
    if (!samples || count == 0 || !obj_) {
        clear();
        return;
    }

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
    y_min_ = std::max<int32_t>(-32768, static_cast<int32_t>(min_v) - pad);
    y_max_ = std::min<int32_t>(32767, static_cast<int32_t>(max_v) + pad);
    if (y_min_ == y_max_) {
        y_min_ = std::max<int32_t>(-32768, y_min_ - 1);
        y_max_ = std::min<int32_t>(32767, y_max_ + 1);
    }

    // Down-sample to column count. A single trace, so the span for each column
    // runs from the sample to the zero line rather than to a stale envelope
    // floor - the old chart had to flatten its lower series for the same
    // reason.
    for (uint16_t i = 0; i < kColumnCount; ++i) {
        uint32_t idx = static_cast<uint32_t>((static_cast<uint64_t>(i) * count) / kColumnCount);
        if (idx >= count)
            idx = count - 1;
        const int16_t v = samples[idx];
        col_min_[i] = std::min<int16_t>(v, 0);
        col_max_[i] = std::max<int16_t>(v, 0);
    }

    has_data_ = true;
    lv_obj_invalidate(obj_);
}

int32_t WaveformView::valueToY(int32_t value, const lv_area_t& area) const {
    const int32_t h = lv_area_get_height(&area);
    const int32_t span = y_max_ - y_min_;
    if (h <= 0 || span <= 0) {
        return area.y1;
    }
    int32_t clamped = std::clamp<int32_t>(value, y_min_, y_max_);
    // y grows downward, so the maximum value sits at the top edge.
    return area.y2 - ((clamped - y_min_) * (h - 1)) / span;
}

void WaveformView::drawEventCb(lv_event_t* e) {
    auto* self = static_cast<WaveformView*>(lv_event_get_user_data(e));
    if (self) {
        self->drawSpans(e);
    }
}

void WaveformView::drawSpans(lv_event_t* e) const {
    lv_layer_t* layer = lv_event_get_layer(e);
    auto* obj = static_cast<lv_obj_t*>(lv_event_get_target(e));
    if (!layer || !obj) {
        return;
    }

    lv_area_t area;
    lv_obj_get_content_coords(obj, &area);
    const int32_t w = lv_area_get_width(&area);
    const int32_t h = lv_area_get_height(&area);
    if (w <= 0 || h <= 0) {
        return;
    }

    lv_draw_rect_dsc_t dsc;
    lv_draw_rect_dsc_init(&dsc);
    dsc.bg_opa = LV_OPA_COVER;
    dsc.radius = 0;
    dsc.border_opa = LV_OPA_TRANSP;
    dsc.outline_opa = LV_OPA_TRANSP;
    dsc.shadow_opa = LV_OPA_TRANSP;

    // Grid first, so the trace sits on top of it as it did with the chart.
    dsc.bg_color = lv_color_hex(kGridColor);
    for (uint8_t r = 1; r < kGridRows; ++r) {
        lv_area_t line = {
            area.x1, area.y1 + (h * r) / kGridRows, area.x2, area.y1 + (h * r) / kGridRows};
        lv_draw_rect(layer, &dsc, &line);
    }
    for (uint8_t c = 1; c < kGridCols; ++c) {
        lv_area_t line = {
            area.x1 + (w * c) / kGridCols, area.y1, area.x1 + (w * c) / kGridCols, area.y2};
        lv_draw_rect(layer, &dsc, &line);
    }

    if (!has_data_) {
        // A zero line, not nothing. The chart this replaced held its two series
        // at 0 when empty, so an unpopulated view still drew a centre line, and
        // dropping that made "no envelope has arrived" indistinguishable from
        // "the panel is broken" - which is exactly how it was reported.
        dsc.bg_color = lv_color_hex(kTraceColor);
        const int32_t y0 = valueToY(0, area);
        lv_area_t zero_line = {area.x1, y0, area.x2, y0};
        lv_draw_rect(layer, &dsc, &zero_line);
        return;
    }

    dsc.bg_color = lv_color_hex(kTraceColor);

    // One rectangle per column would be kColumnCount draw tasks. Runs of
    // columns that map to the same span - silence, sustained tones, anything
    // flat - collapse into one, which is most of a typical waveform's width.
    uint16_t run_start = 0;
    for (uint16_t i = 1; i <= kColumnCount; ++i) {
        const bool same = i < kColumnCount && col_min_[i] == col_min_[run_start] &&
                          col_max_[i] == col_max_[run_start];
        if (same) {
            continue;
        }

        const int32_t x1 = area.x1 + (w * run_start) / kColumnCount;
        const int32_t x2 = area.x1 + (w * i) / kColumnCount - 1;

        int32_t y_top = valueToY(col_max_[run_start], area);
        int32_t y_bot = valueToY(col_min_[run_start], area);
        if (y_bot < y_top) {
            std::swap(y_top, y_bot);
        }
        // lv_area_t bounds are inclusive, so a column whose min and max
        // collapse (silence, DC) still draws as a 1 px line with no special
        // casing - the grid lines above rely on the same property.
        lv_area_t span = {x1, y_top, std::max(x1, x2), y_bot};
        lv_draw_rect(layer, &dsc, &span);

        run_start = i;
    }
}

}  // namespace wavex_ui
