#include "waveform_view.h"

#include "ui_theme.h"

#include <algorithm>

namespace wavex_ui {
namespace {

// Full-scale int16. Fixed: see setEnvelope() on why this never autoscales.
constexpr int32_t kYMin = -32768;
constexpr int32_t kYMax = 32767;

}  // namespace

WaveformView::WaveformView(lv_obj_t* parent, int32_t width, int32_t height) {
    if (!LV_COORD_IS_PCT(width) && width != LV_SIZE_CONTENT && width > 0) {
        columns_ =
            static_cast<uint16_t>(std::min<int32_t>(width, WaveX::Protocol::MAX_ENVELOPE_COLUMNS));
    }
    col_min_.assign(static_cast<size_t>(columns_) * kMaxChannels, 0);
    col_max_.assign(static_cast<size_t>(columns_) * kMaxChannels, 0);

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
    std::fill(col_min_.begin(), col_min_.end(), 0);
    std::fill(col_max_.begin(), col_max_.end(), 0);
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

    // The wire's stride is whatever the backend sent; only the lanes we can
    // draw are stored. Clamping the STORED count while indexing with the
    // original stride is what keeps a 3-channel payload reading channels 0 and
    // 1 rather than interleaved nonsense.
    const uint8_t stride = channels;
    channels_ = (channels > kMaxChannels) ? kMaxChannels : channels;

    if (count == columns_) {
        // The panel's case: nothing to resample.
        for (uint16_t i = 0; i < columns_; ++i) {
            for (uint8_t ch = 0; ch < channels_; ++ch) {
                const WaveX::Protocol::EnvelopeColumn& c = columns[i * stride + ch];
                colMin(ch, i) = c.min_sample;
                colMax(ch, i) = c.max_sample;
            }
        }
    } else if (count > columns_) {
        // Reduce by merging: every source column contributes its extremes to
        // the display column it lands in, so nothing narrower than a column
        // can disappear. Same split as the cache's render, so a view fed a
        // finer tier looks like one fed its own.
        for (uint16_t i = 0; i < columns_; ++i) {
            const uint32_t s0 = (static_cast<uint32_t>(i) * count) / columns_;
            uint32_t s1 = (static_cast<uint32_t>(i + 1) * count) / columns_;
            if (s1 <= s0) {
                s1 = s0 + 1;
            }
            for (uint8_t ch = 0; ch < channels_; ++ch) {
                int16_t lo = columns[s0 * stride + ch].min_sample;
                int16_t hi = columns[s0 * stride + ch].max_sample;
                for (uint32_t s = s0 + 1; s < s1; ++s) {
                    const WaveX::Protocol::EnvelopeColumn& c = columns[s * stride + ch];
                    lo = std::min(lo, c.min_sample);
                    hi = std::max(hi, c.max_sample);
                }
                colMin(ch, i) = lo;
                colMax(ch, i) = hi;
            }
        }
    } else {
        // Stretch: fewer columns than pixels, each source column repeats.
        for (uint16_t i = 0; i < columns_; ++i) {
            uint32_t idx = (static_cast<uint32_t>(i) * count) / columns_;
            if (idx >= count) {
                idx = count - 1;
            }
            for (uint8_t ch = 0; ch < channels_; ++ch) {
                const WaveX::Protocol::EnvelopeColumn& c = columns[idx * stride + ch];
                colMin(ch, i) = c.min_sample;
                colMax(ch, i) = c.max_sample;
            }
        }
    }

    has_data_ = true;
    lv_obj_invalidate(obj_);
}

int32_t WaveformView::valueToY(int32_t value, const lv_area_t& area) {
    const int32_t h = lv_area_get_height(&area);
    constexpr int32_t span = kYMax - kYMin;
    if (h <= 0) {
        return area.y1;
    }
    const int32_t clamped = std::clamp<int32_t>(value, kYMin, kYMax);
    // y grows downward, so the maximum value sits at the top edge.
    return area.y2 - ((clamped - kYMin) * (h - 1)) / span;
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

// Only fills that reach the clip become draw tasks. LVGL renders the screen
// in WAVEX_LVGL_DRAW_BUF_HEIGHT-line strips and raises LV_EVENT_DRAW_MAIN
// once per strip an object touches, with the clip set to that strip; a task
// outside it is malloc'd, appended to the layer's list by walking to its tail,
// and only then discarded at dispatch. Handing over every column on every
// strip made one 1230-column stereo frame ~18 strips x ~12 ms of UI-task
// time - and a marker moving over the trace invalidates a sliver that costs
// the same. With the clip honoured and runs merged on their clipped spans
// (drawLane) the same frame is ~37 ms, no strip over ~3.5 ms. Bench,
// 2026-09-06, 44 s stereo file.
static inline bool touches(const lv_area_t& a, const lv_area_t& clip) {
    return a.x2 >= clip.x1 && a.x1 <= clip.x2 && a.y2 >= clip.y1 && a.y1 <= clip.y2;
}

static inline void fillClipped(lv_layer_t* layer,
                               const lv_draw_fill_dsc_t& dsc,
                               const lv_area_t& a,
                               const lv_area_t& clip) {
    if (touches(a, clip)) {
        lv_draw_fill(layer, &dsc, &a);
    }
}

void WaveformView::drawLane(lv_layer_t* layer,
                            lv_draw_fill_dsc_t& dsc,
                            const lv_area_t& lane,
                            const lv_area_t& clip,
                            uint8_t channel) const {
    const int32_t w = lv_area_get_width(&lane);
    if (w <= 0 || lv_area_get_height(&lane) <= 0 || !touches(lane, clip)) {
        return;
    }

    const int16_t* mins = &col_min_[static_cast<size_t>(channel) * columns_];
    const int16_t* maxs = &col_max_[static_cast<size_t>(channel) * columns_];

    // One fill per column would be columns_ draw tasks. Runs of columns whose
    // span is the same AFTER clipping collapse into one: silence and anything
    // flat, as before, and - because the clip is a 40-line strip - every
    // column a loud passage drives through the whole strip, which is most of
    // them where the trace is dense. A column with nothing in the strip adds
    // no task at all. lv_area_t bounds are inclusive, so a column whose min
    // and max collapse (silence, DC) still draws as a 1 px line with no
    // special casing - the grid lines rely on the same property.
    const int32_t clip_top = std::max(clip.y1, lane.y1);
    const int32_t clip_bot = std::min(clip.y2, lane.y2);
    if (clip_bot < clip_top) {
        return;
    }
    const int32_t col0 = std::max<int32_t>(0, ((clip.x1 - lane.x1) * columns_) / w);
    const int32_t col1 = std::min<int32_t>(columns_ - 1, ((clip.x2 - lane.x1) * columns_) / w + 1);

    int32_t run_start = -1;
    int32_t run_top = 0;
    int32_t run_bot = 0;
    auto flush = [&](int32_t end) {
        if (run_start < 0) {
            return;
        }
        const int32_t x1 = lane.x1 + (w * run_start) / columns_;
        const int32_t x2 = lane.x1 + (w * end) / columns_ - 1;
        lv_area_t span = {x1, run_top, std::max(x1, x2), run_bot};
        lv_draw_fill(layer, &dsc, &span);
        run_start = -1;
    };
    for (int32_t i = col0; i <= col1; ++i) {
        int32_t y_top = valueToY(maxs[i], lane);
        int32_t y_bot = valueToY(mins[i], lane);
        if (y_bot < y_top) {
            std::swap(y_top, y_bot);
        }
        y_top = std::max(y_top, clip_top);
        y_bot = std::min(y_bot, clip_bot);
        if (y_bot < y_top) {
            flush(i);
            continue;
        }
        if (run_start >= 0 && y_top == run_top && y_bot == run_bot) {
            continue;
        }
        flush(i);
        run_start = i;
        run_top = y_top;
        run_bot = y_bot;
    }
    flush(col1 + 1);
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
    // What this event is actually for: the strip being rendered, or the
    // invalidated sliver. This is the clip every task added now inherits.
    const lv_area_t clip = layer->_clip_area;
    if (!touches(area, clip)) {
        return;
    }

    const bool stereo = channels_ > 1;
    const uint8_t grid_rows = stereo ? kGridRowsStereo : kGridRowsMono;

    // A plain fill, not lv_draw_rect: the rect path re-decides border, shadow,
    // outline and background image for every one of up to columns_ calls.
    lv_draw_fill_dsc_t dsc;
    lv_draw_fill_dsc_init(&dsc);
    dsc.opa = LV_OPA_COVER;
    dsc.radius = 0;

    // Grid first, so the traces sit on top of it as they did with the chart.
    // Columns span the full height - they mark time, which both channels
    // share - while rows are drawn per lane so each trace keeps a line on its
    // own zero.
    dsc.color = UI_COLOR_LINE;
    for (uint8_t c = 1; c < kGridCols; ++c) {
        lv_area_t line = {
            area.x1 + (w * c) / kGridCols, area.y1, area.x1 + (w * c) / kGridCols, area.y2};
        fillClipped(layer, dsc, line, clip);
    }
    for (uint8_t ch = 0; ch < channels_; ++ch) {
        const lv_area_t lane = laneArea(area, ch);
        const int32_t lane_h = lv_area_get_height(&lane);
        for (uint8_t r = 1; r < grid_rows; ++r) {
            const int32_t y = lane.y1 + (lane_h * r) / grid_rows;
            lv_area_t line = {lane.x1, y, lane.x2, y};
            fillClipped(layer, dsc, line, clip);
        }
    }

    if (stereo) {
        // Brighter than the grid so the split between L and R does not read
        // as just another div line - it is a boundary between two signals,
        // not a scale mark.
        const int32_t mid = area.y1 + h / 2;
        dsc.color = UI_COLOR_DIMMER;
        lv_area_t divider = {area.x1, mid, area.x2, mid};
        fillClipped(layer, dsc, divider, clip);
    }

    if (!has_data_) {
        // A zero line, not nothing. The chart this replaced held its two series
        // at 0 when empty, so an unpopulated view still drew a centre line, and
        // dropping that made "no envelope has arrived" indistinguishable from
        // "the panel is broken" - which is exactly how it was reported.
        dsc.color = UI_COLOR_ACCENT;
        const int32_t y0 = valueToY(0, area);
        lv_area_t zero_line = {area.x1, y0, area.x2, y0};
        fillClipped(layer, dsc, zero_line, clip);
        return;
    }

    dsc.color = UI_COLOR_ACCENT;
    for (uint8_t ch = 0; ch < channels_; ++ch) {
        drawLane(layer, dsc, laneArea(area, ch), clip, ch);
    }

    // Name the lanes. Without this the panel cannot be told apart from the
    // single-trace version it replaced, and "which channel am I looking at"
    // going unanswered is the exact defect roadmap 1.5.7 opens with.
    if (stereo) {
        lv_draw_label_dsc_t label_dsc;
        lv_draw_label_dsc_init(&label_dsc);
        label_dsc.color = UI_COLOR_DIM;
        label_dsc.font = UI_FONT_MICRO;
        label_dsc.opa = LV_OPA_COVER;

        static const char* const kNames[kMaxChannels] = {"L", "R"};
        for (uint8_t ch = 0; ch < channels_; ++ch) {
            const lv_area_t lane = laneArea(area, ch);
            label_dsc.text = kNames[ch];
            lv_area_t at = {lane.x1 + 4, lane.y1 + 2, lane.x1 + 24, lane.y1 + 20};
            if (touches(at, clip)) {
                lv_draw_label(layer, &label_dsc, &at);
            }
        }
    }
}

}  // namespace wavex_ui
