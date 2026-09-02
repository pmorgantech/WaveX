#include "waveform_view.h"

#include <algorithm>

namespace wavex_ui {
namespace {

// The trace colour the chart used, kept so the look does not change with the
// drawing method.
constexpr uint32_t kTraceColor = 0x29B6F6;  // LV_PALETTE_LIGHT_BLUE main
constexpr uint32_t kGridColor = 0x2A2A2A;

// Brighter than the grid so the split between L and R does not read as just
// another div line - it is a boundary between two signals, not a scale mark.
constexpr uint32_t kDividerColor = 0x4A4A4A;
constexpr uint32_t kChannelLabelColor = 0x8A8A8A;

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
    for (auto& lane: col_min_) {
        lane.fill(0);
    }
    for (auto& lane: col_max_) {
        lane.fill(0);
    }
    y_min_ = -32768;
    y_max_ = 32767;
    channels_ = 1;
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

    // The wire's stride is whatever the backend sent; only the lanes we can
    // draw are stored. Clamping the STORED count while indexing with the
    // original stride is what keeps a 3-channel payload reading channels 0 and
    // 1 rather than interleaved nonsense.
    const uint8_t stride = channels;
    channels_ = (channels > kMaxChannels) ? kMaxChannels : channels;

    for (uint16_t i = 0; i < kColumnCount; ++i) {
        uint32_t idx = static_cast<uint32_t>((static_cast<uint64_t>(i) * count) / kColumnCount);
        if (idx >= count) {
            idx = count - 1;
        }

        for (uint8_t ch = 0; ch < channels_; ++ch) {
            const WaveX::Protocol::EnvelopeColumn& c = columns[idx * stride + ch];
            col_min_[ch][i] = c.min_sample;
            col_max_[ch][i] = c.max_sample;
        }
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

    // Raw samples are mono by definition here (the record page's preview), so
    // there is one lane and it uses the full height.
    channels_ = 1;

    // Down-sample to column count. A single trace, so the span for each column
    // runs from the sample to the zero line rather than to a stale envelope
    // floor - the old chart had to flatten its lower series for the same
    // reason.
    for (uint16_t i = 0; i < kColumnCount; ++i) {
        uint32_t idx = static_cast<uint32_t>((static_cast<uint64_t>(i) * count) / kColumnCount);
        if (idx >= count)
            idx = count - 1;
        const int16_t v = samples[idx];
        col_min_[0][i] = std::min<int16_t>(v, 0);
        col_max_[0][i] = std::max<int16_t>(v, 0);
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

lv_area_t WaveformView::laneArea(const lv_area_t& content, uint8_t channel) const {
    if (channels_ <= 1) {
        return content;
    }
    // The divider occupies the middle row and belongs to neither lane, so a
    // trace at full deflection cannot be mistaken for the boundary.
    const int32_t mid = content.y1 + lv_area_get_height(&content) / 2;
    if (channel == 0) {
        return {content.x1, content.y1, content.x2, mid - 1};
    }
    return {content.x1, mid + 1, content.x2, content.y2};
}

void WaveformView::drawLane(lv_layer_t* layer,
                            lv_draw_rect_dsc_t& dsc,
                            const lv_area_t& lane,
                            uint8_t channel) const {
    const int32_t w = lv_area_get_width(&lane);
    if (w <= 0 || lv_area_get_height(&lane) <= 0) {
        return;
    }

    const std::array<int16_t, kColumnCount>& mins = col_min_[channel];
    const std::array<int16_t, kColumnCount>& maxs = col_max_[channel];

    // One rectangle per column would be kColumnCount draw tasks. Runs of
    // columns that map to the same span - silence, sustained tones, anything
    // flat - collapse into one, which is most of a typical waveform's width.
    uint16_t run_start = 0;
    for (uint16_t i = 1; i <= kColumnCount; ++i) {
        const bool same =
            i < kColumnCount && mins[i] == mins[run_start] && maxs[i] == maxs[run_start];
        if (same) {
            continue;
        }

        const int32_t x1 = lane.x1 + (w * run_start) / kColumnCount;
        const int32_t x2 = lane.x1 + (w * i) / kColumnCount - 1;

        int32_t y_top = valueToY(maxs[run_start], lane);
        int32_t y_bot = valueToY(mins[run_start], lane);
        if (y_bot < y_top) {
            std::swap(y_top, y_bot);
        }
        // lv_area_t bounds are inclusive, so a column whose min and max
        // collapse (silence, DC) still draws as a 1 px line with no special
        // casing - the grid lines rely on the same property.
        lv_area_t span = {x1, y_top, std::max(x1, x2), y_bot};
        lv_draw_rect(layer, &dsc, &span);

        run_start = i;
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

    const bool stereo = channels_ > 1;
    const uint8_t grid_rows = stereo ? kGridRowsStereo : kGridRowsMono;

    lv_draw_rect_dsc_t dsc;
    lv_draw_rect_dsc_init(&dsc);
    dsc.bg_opa = LV_OPA_COVER;
    dsc.radius = 0;
    dsc.border_opa = LV_OPA_TRANSP;
    dsc.outline_opa = LV_OPA_TRANSP;
    dsc.shadow_opa = LV_OPA_TRANSP;

    // Grid first, so the traces sit on top of it as they did with the chart.
    // Columns span the full height - they mark time, which both channels
    // share - while rows are drawn per lane so each trace keeps a line on its
    // own zero.
    dsc.bg_color = lv_color_hex(kGridColor);
    for (uint8_t c = 1; c < kGridCols; ++c) {
        lv_area_t line = {
            area.x1 + (w * c) / kGridCols, area.y1, area.x1 + (w * c) / kGridCols, area.y2};
        lv_draw_rect(layer, &dsc, &line);
    }
    for (uint8_t ch = 0; ch < channels_; ++ch) {
        const lv_area_t lane = laneArea(area, ch);
        const int32_t lane_h = lv_area_get_height(&lane);
        for (uint8_t r = 1; r < grid_rows; ++r) {
            const int32_t y = lane.y1 + (lane_h * r) / grid_rows;
            lv_area_t line = {lane.x1, y, lane.x2, y};
            lv_draw_rect(layer, &dsc, &line);
        }
    }

    if (stereo) {
        const int32_t mid = area.y1 + h / 2;
        dsc.bg_color = lv_color_hex(kDividerColor);
        lv_area_t divider = {area.x1, mid, area.x2, mid};
        lv_draw_rect(layer, &dsc, &divider);
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
    for (uint8_t ch = 0; ch < channels_; ++ch) {
        drawLane(layer, dsc, laneArea(area, ch), ch);
    }

    // Name the lanes. Without this the panel cannot be told apart from the
    // single-trace version it replaced, and "which channel am I looking at"
    // going unanswered is the exact defect roadmap 1.5.7 opens with.
    if (stereo) {
        lv_draw_label_dsc_t label_dsc;
        lv_draw_label_dsc_init(&label_dsc);
        label_dsc.color = lv_color_hex(kChannelLabelColor);
        label_dsc.font = &lv_font_montserrat_14;
        label_dsc.opa = LV_OPA_COVER;

        static const char* const kNames[kMaxChannels] = {"L", "R"};
        for (uint8_t ch = 0; ch < channels_; ++ch) {
            const lv_area_t lane = laneArea(area, ch);
            label_dsc.text = kNames[ch];
            lv_area_t at = {lane.x1 + 4, lane.y1 + 2, lane.x1 + 24, lane.y1 + 20};
            lv_draw_label(layer, &label_dsc, &at);
        }
    }
}

}  // namespace wavex_ui
