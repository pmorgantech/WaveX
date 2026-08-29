#include "ui/ui_sample_edit_page.h"

#include <esp_log.h>
#include <esp_timer.h>

#include "components/waveform_view.h"
#include "inter_mcu.h"
#include "ui/ui_navigator.h"
#include "ui/ui_sample_browser.h"

#include <algorithm>
#include <cstdio>
#include <string>

namespace wavex_ui {

namespace {

constexpr uint8_t kPreviewSlot = 0;
// Fallback only, used when the browse listing carried no usable duration.
// This used to be the assumed sample length, which is exactly one second at
// 48 kHz - the reason markers would not move past 1 s and zoom would not open
// out. Real geometry now comes from SampleBrowserState.
constexpr uint32_t kFallbackFrames = 48000;
constexpr uint16_t kPreviewPoints = 512;
// Hard ceiling on the preview buffer. It is allocated once at this size and
// never resized, because the UART task writes into it while the UI task reads
// it: a reallocation mid-copy is a use-after-free, where a torn value is just
// one stale frame. Decimation is chosen so a window yields at most this many
// points, and any reply claiming more is dropped.
constexpr uint16_t kMaxPreviewPoints = 1024;
constexpr uint32_t kMinWindow = 256;

// Coalescing window for encoder-driven preview requests. Long enough that a
// continuous turn produces one request rather than one per detent, short
// enough that the waveform still feels like it is tracking the knob.
constexpr uint32_t kRequestSettleMs = 150;

// Layout, page-relative (the navigator's content area already starts below the
// 75px header). Design 2e: waveform 1256x250 @ y12, param strip y278, info y434.
constexpr int kMargin = 12;
constexpr int kWaveW = 1256;
constexpr int kWaveY = 12;
constexpr int kWaveH = 250;
constexpr int kStripY = 278;
constexpr int kCardW = 305;
constexpr int kCardH = 132;
constexpr int kCardPitch = 317;
constexpr int kGaugeW = 273;
constexpr int kInfoY = 434;
constexpr int kInfoH = 88;

constexpr uint32_t kColCard = 0x141414;
constexpr uint32_t kColBorder = 0x2A2A2A;
constexpr uint32_t kColDim = 0x8FA0AA;
constexpr uint32_t kColDimmer = 0x5A6670;
constexpr uint32_t kColBlue = 0x2196F3;
constexpr uint32_t kColGreen = 0x4CAF50;
constexpr uint32_t kColOrange = 0xFF9800;

const char* TAG = "UI_SAMPLE_EDIT";

lv_obj_t* box(lv_obj_t* parent, int x, int y, int w, int h, uint32_t colour) {
    lv_obj_t* o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_size(o, w, h);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_style_bg_color(o, lv_color_hex(colour), 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    return o;
}

lv_obj_t* label(
    lv_obj_t* parent, int x, int y, const char* text, const lv_font_t* font, uint32_t colour) {
    lv_obj_t* l = lv_label_create(parent);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(colour), 0);
    lv_obj_set_pos(l, x, y);
    return l;
}

// mm:ss.mmm from a frame count at the sample's own rate.
void formatFrames(char* out, size_t n, uint32_t frames, uint32_t rate) {
    if (rate == 0) {
        rate = 48000;
    }
    const uint32_t ms = static_cast<uint32_t>((frames * 1000ULL) / rate);
    snprintf(out,
             n,
             "%lu:%02lu.%03lu",
             static_cast<unsigned long>(ms / 60000),
             static_cast<unsigned long>((ms / 1000) % 60),
             static_cast<unsigned long>(ms % 1000));
}

}  // namespace

void UISampleEditPage::onEnter(lv_obj_t* parent) {
    root_ = lv_obj_create(parent);
    lv_obj_remove_style_all(root_);
    lv_obj_set_size(root_, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(root_, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(root_, LV_OPA_COVER, 0);
    lv_obj_remove_flag(root_, LV_OBJ_FLAG_SCROLLABLE);

    // Allocated once, before any chunk can arrive, and never resized after.
    preview_buffer_.assign(kMaxPreviewPoints, 0);

    buildWaveformPanel(root_);
    buildParamStrip(root_);
    buildInfoStrip(root_);

    ui_timer_ = lv_timer_create(&UISampleEditPage::uiTimerCb, 50, this);
    // Only after the timer exists: a chunk arriving before it would set a flag
    // nothing is watching, and the first waveform would never be drawn.
    inter_mcu_set_wave_chunk_listener(&UISampleEditPage::waveChunkStatic, this);

    auto* state = getSampleBrowserState();
    has_sample_ = state && !state->last_load_sample_path.empty();
    if (!has_sample_) {
        refreshStatus("Load a sample in Sample Browser, then reopen Edit.");
        refreshParams();
        return;
    }

    // Real geometry from the browse listing. Opens fully zoomed out with the
    // region spanning the whole clip, which is the only starting point from
    // which every marker is reachable.
    sample_rate_ = state->last_load_sample_rate ? state->last_load_sample_rate : 48000;
    total_frames_ = state->lastLoadFrames();
    if (total_frames_ == 0) {
        total_frames_ = kFallbackFrames;
        ESP_LOGW(TAG,
                 "No duration in browse metadata; falling back to %lu frames",
                 (unsigned long)total_frames_);
    }
    start_frame_ = 0;
    end_frame_ = total_frames_;
    loop_start_ = 0;
    loop_end_ = total_frames_;
    loop_enabled_ = false;
    gain_db_x10_ = 0;
    zoomToFit();
    expected_len_ = kPreviewPoints;

    refreshStatus(state->last_load_sample_path.c_str());
    refreshParams();
    requestWaveform();
}

void UISampleEditPage::buildWaveformPanel(lv_obj_t* parent) {
    lv_obj_t* panel = box(parent, kMargin, kWaveY, kWaveW, kWaveH, 0x101010);
    lv_obj_set_style_border_width(panel, 1, 0);
    lv_obj_set_style_border_color(panel, lv_color_hex(kColBorder), 0);
    lv_obj_set_style_pad_all(panel, 4, 0);

    waveform_ = std::make_unique<WaveformView>(panel, lv_pct(100), lv_pct(100));

    // Region handles: 30x26 tabs on the top edge, S green / E orange, matching
    // the marker colours the browser's preview already uses.
    marker_s_ = box(parent, kMargin, kWaveY, 30, 26, kColGreen);
    lv_obj_t* ls = label(marker_s_, 0, 0, "S", &lv_font_montserrat_18, 0x0A0A0A);
    lv_obj_center(ls);
    marker_e_ = box(parent, kMargin + kWaveW - 30, kWaveY, 30, 26, kColOrange);
    lv_obj_t* le = label(marker_e_, 0, 0, "E", &lv_font_montserrat_18, 0x0A0A0A);
    lv_obj_center(le);

    // Loop handles on the bottom edge, so they never overlap S/E even when a
    // loop sits exactly on the region bounds - which is the default.
    marker_ls_ = box(parent, kMargin, kWaveY + kWaveH - 26, 34, 26, kColBlue);
    lv_obj_t* lls = label(marker_ls_, 0, 0, "LS", &lv_font_montserrat_14, 0x0A0A0A);
    lv_obj_center(lls);
    marker_le_ = box(parent, kMargin + kWaveW - 34, kWaveY + kWaveH - 26, 34, 26, kColBlue);
    lv_obj_t* lle = label(marker_le_, 0, 0, "LE", &lv_font_montserrat_14, 0x0A0A0A);
    lv_obj_center(lle);
}

void UISampleEditPage::buildParamStrip(lv_obj_t* parent) {
    static const char* titles[PARAM_COUNT] = {"START", "END", "LOOP START", "LOOP END", "GAIN"};
    // Five parameters, four card slots. The strip shows a window onto the
    // parameter list so the design's 305px card pitch survives; < Param /
    // Param > scroll it. Cramming five into the same width would shrink every
    // card below the readable-from-a-metre size the layout is built around.
    for (int i = 0; i < PARAM_COUNT; i++) {
        lv_obj_t* c = box(parent, kMargin, kStripY, kCardW, kCardH, kColCard);
        lv_obj_set_style_border_width(c, 1, 0);
        lv_obj_set_style_border_color(c, lv_color_hex(kColBorder), 0);
        lv_obj_set_style_radius(c, 4, 0);

        label(c, 16, 12, titles[i], &lv_font_montserrat_18, kColDim);

        cards_[i].card = c;
        cards_[i].value = label(c, 16, 40, "0:00.000", &lv_font_montserrat_32, 0xFFFFFF);
        box(c, 16, 96, kGaugeW, 14, 0x1F1F1F);
        cards_[i].bar = box(c, 16, 96, 0, 14, kColBlue);
        cards_[i].knob = box(c, 12, 92, 8, 22, 0xFFFFFF);
    }
    refreshFocusRing();
}

// Places the visible window of cards so the focused one is always on screen.
void UISampleEditPage::layoutParamStrip() {
    int first = focus_ - (kVisibleCards - 1);
    if (first < 0) {
        first = 0;
    }
    if (first > PARAM_COUNT - kVisibleCards) {
        first = PARAM_COUNT - kVisibleCards;
    }
    if (focus_ < first) {
        first = focus_;
    }
    for (int i = 0; i < PARAM_COUNT; i++) {
        const int slot = i - first;
        if (slot < 0 || slot >= kVisibleCards) {
            lv_obj_add_flag(cards_[i].card, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_remove_flag(cards_[i].card, LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_x(cards_[i].card, kMargin + slot * kCardPitch);
        }
    }
}

void UISampleEditPage::buildInfoStrip(lv_obj_t* parent) {
    lv_obj_t* info = box(parent, kMargin, kInfoY, kWaveW, kInfoH, kColCard);
    lv_obj_set_style_border_width(info, 1, 0);
    lv_obj_set_style_border_color(info, lv_color_hex(kColBorder), 0);
    lv_obj_set_style_radius(info, 4, 0);

    info_label_ = label(info, 16, 14, "", &lv_font_montserrat_18, kColDim);
    status_label_ = label(info, 16, 48, "", &lv_font_montserrat_18, kColGreen);
    lv_obj_set_width(status_label_, kWaveW - 32);
    lv_label_set_long_mode(status_label_, LV_LABEL_LONG_DOT);
}

void UISampleEditPage::onExit() {
    // Unregister first. Deleting the timer or the widgets while a chunk can
    // still arrive would leave the RX task writing through a freed page.
    inter_mcu_set_wave_chunk_listener(nullptr, nullptr);
    if (auditioning_) {
        inter_mcu_send_sample_stop_req();
        auditioning_ = false;
    }
    if (ui_timer_) {
        lv_timer_delete(ui_timer_);
        ui_timer_ = nullptr;
    }
    waveform_.reset();
    if (root_) {
        lv_obj_del(root_);
        root_ = nullptr;
    }
    status_label_ = nullptr;
    info_label_ = nullptr;
    marker_s_ = nullptr;
    marker_e_ = nullptr;
    for (auto& c: cards_) {
        c = ParamCard{};
    }
}

void UISampleEditPage::onInput(const InputEvent& evt) {
    // evt.delta is already SIGNED, and the event type only names the sign.
    // Negating it for the Left/Down case therefore flipped it back to
    // positive, which is why counter-clockwise increased the value. Take the
    // magnitude and let the event type supply the direction, as the sample
    // browser does.
    const int steps = evt.delta < 0 ? -evt.delta : (evt.delta ? evt.delta : 1);
    switch (evt.type) {
        case InputType::EncoderRight:
        case InputType::EncoderUp:
            adjustFocused(steps);
            break;
        case InputType::EncoderLeft:
        case InputType::EncoderDown:
            adjustFocused(-steps);
            break;
        case InputType::EncoderClick:
            focus_ = static_cast<uint8_t>((focus_ + 1) % PARAM_COUNT);
            refreshFocusRing();
            break;
        default:
            break;
    }
}

std::array<Softkey, NUM_SOFTKEYS> UISampleEditPage::getSoftkeys() {
    std::array<Softkey, NUM_SOFTKEYS> keys{};
    keys[0] = {"Back", []() { UINavigator::instance().pop(); }};
    // Toggles to Stop while playing, matching the sample browser. Two keys for
    // one mutually-exclusive action would waste a slot on a row that is
    // already short.
    keys[1] = {auditioning_ ? "Stop" : "Audition", [this]() { toggleAudition(); }};
    keys[2] = {"Zoom -", [this]() { setZoom(-1); }};
    keys[3] = {"Zoom +", [this]() { setZoom(1); }};
    keys[4] = {"< Param", [this]() {
                   focus_ = static_cast<uint8_t>((focus_ + PARAM_COUNT - 1) % PARAM_COUNT);
                   refreshFocusRing();
               }};
    keys[5] = {"Param >", [this]() {
                   focus_ = static_cast<uint8_t>((focus_ + 1) % PARAM_COUNT);
                   refreshFocusRing();
               }};
    return keys;
}

// Shifted row: the file- and parameter-level operations. All of these need
// protocol work that does not exist yet (roadmap Phase 1.5.1), so they are
// present but disabled with the reason attached - a key that silently does
// nothing is worse than one that says why it cannot.
std::array<Softkey, NUM_SOFTKEYS> UISampleEditPage::getShiftedSoftkeys() {
    std::array<Softkey, NUM_SOFTKEYS> keys{};
    keys[0] = {"Select", nullptr, false, "no sample picker yet"};
    keys[1] = {loop_enabled_ ? "Loop Off" : "Loop On", [this]() {
                   loop_enabled_ = !loop_enabled_;
                   params_dirty_ = true;
                   sendEdit();
                   UINavigator::instance().refreshSoftkeys();
               }};
    keys[2] = {"Gain 0dB", [this]() {
                   gain_db_x10_ = 0;
                   params_dirty_ = true;
                   sendEdit();
               }};
    keys[3] = {"Save", nullptr, false, "needs marker persistence"};
    keys[4] = {"Save As", nullptr, false, "needs filename entry"};
    keys[5] = {"Reset", [this]() {
                   start_frame_ = 0;
                   end_frame_ = total_frames_;
                   loop_start_ = 0;
                   loop_end_ = total_frames_;
                   gain_db_x10_ = 0;
                   zoomToFit();
                   params_dirty_ = true;
                   sendEdit();
                   request_due_ms_ = (uint32_t)(esp_timer_get_time() / 1000) + kRequestSettleMs;
               }};
    return keys;
}

void UISampleEditPage::toggleAudition() {
    auto* state = getSampleBrowserState();
    if (!state) {
        return;
    }
    if (auditioning_) {
        if (inter_mcu_send_sample_stop_req() == ESP_OK) {
            auditioning_ = false;
            refreshStatus("Stopped");
        } else {
            refreshStatus("Stop request failed");
        }
    } else {
        if (inter_mcu_send_sample_play_index_req(state->selected_file_index) == ESP_OK) {
            auditioning_ = true;
            refreshStatus("Auditioning whole file (range not sent - no protocol)");
        } else {
            refreshStatus("Audition request failed");
        }
    }
    UINavigator::instance().refreshSoftkeys();
}

void UISampleEditPage::clampMarkers() {
    // Same ordering the backend enforces, applied here too so the display
    // never shows a region the engine would silently reject. The backend
    // stays the authority; this only keeps the UI honest about it.
    if (total_frames_ == 0) {
        return;
    }
    if (end_frame_ > total_frames_) {
        end_frame_ = total_frames_;
    }
    if (start_frame_ >= end_frame_) {
        start_frame_ = end_frame_ > 0 ? end_frame_ - 1 : 0;
    }
    if (loop_end_ > end_frame_) {
        loop_end_ = end_frame_;
    }
    if (loop_start_ < start_frame_) {
        loop_start_ = start_frame_;
    }
    if (loop_start_ >= loop_end_) {
        loop_start_ = loop_end_ > start_frame_ ? loop_end_ - 1 : start_frame_;
    }
}

void UISampleEditPage::adjustFocused(int steps) {
    if (!has_sample_ || total_frames_ == 0) {
        return;
    }
    if (focus_ == PARAM_GAIN) {
        // 0.5 dB a detent over -24..+12 dB.
        int32_t g = gain_db_x10_ + steps * 5;
        if (g < -240) {
            g = -240;
        }
        if (g > 120) {
            g = 120;
        }
        gain_db_x10_ = static_cast<int16_t>(g);
        params_dirty_ = true;
        sendEdit();
        return;
    }

    // One detent moves 1/200th of the VISIBLE span, so the same gesture is
    // coarse when zoomed out and fine when zoomed in. Scaling by the whole
    // sample instead would make a three-minute file unadjustable.
    const uint32_t span = view_frames_ ? view_frames_ : total_frames_;
    const int32_t step = static_cast<int32_t>(span / 200) + 1;
    const int64_t delta = static_cast<int64_t>(steps) * step;

    uint32_t* target = nullptr;
    switch (focus_) {
        case PARAM_START:
            target = &start_frame_;
            break;
        case PARAM_END:
            target = &end_frame_;
            break;
        case PARAM_LOOP_START:
            target = &loop_start_;
            break;
        case PARAM_LOOP_END:
            target = &loop_end_;
            break;
        default:
            return;
    }
    int64_t v = static_cast<int64_t>(*target) + delta;
    if (v < 0) {
        v = 0;
    }
    if (v > static_cast<int64_t>(total_frames_)) {
        v = total_frames_;
    }
    *target = static_cast<uint32_t>(v);
    clampMarkers();

    // Follow the marker if it leaves the visible span, otherwise adjusting a
    // marker while zoomed in silently pushes it off-screen.
    if (*target < view_start_ || *target > view_start_ + view_frames_) {
        const uint32_t half = view_frames_ / 2;
        view_start_ = (*target > half) ? (*target - half) : 0;
        if (view_start_ + view_frames_ > total_frames_) {
            view_start_ = (total_frames_ > view_frames_) ? total_frames_ - view_frames_ : 0;
        }
        request_due_ms_ = (uint32_t)(esp_timer_get_time() / 1000) + kRequestSettleMs;
    }

    params_dirty_ = true;
    sendEdit();
}

void UISampleEditPage::zoomToFit() {
    view_start_ = 0;
    view_frames_ = total_frames_;
}

void UISampleEditPage::setZoom(int direction) {
    if (!has_sample_ || total_frames_ == 0) {
        return;
    }
    // Anchor on the focused marker, not on the view centre: zooming in is
    // nearly always "show me more detail around the thing I am moving".
    uint32_t anchor = view_start_ + view_frames_ / 2;
    switch (focus_) {
        case PARAM_START:
            anchor = start_frame_;
            break;
        case PARAM_END:
            anchor = end_frame_;
            break;
        case PARAM_LOOP_START:
            anchor = loop_start_;
            break;
        case PARAM_LOOP_END:
            anchor = loop_end_;
            break;
        default:
            break;
    }

    if (direction > 0) {
        view_frames_ = std::max(kMinWindow, view_frames_ / 2);
    } else {
        // Out is capped at the whole sample - it used to be capped at a fixed
        // 48000 frames, which is why it appeared not to zoom out at all on
        // anything longer than a second.
        view_frames_ = std::min(total_frames_, view_frames_ * 2);
    }

    const uint32_t half = view_frames_ / 2;
    view_start_ = (anchor > half) ? (anchor - half) : 0;
    if (view_start_ + view_frames_ > total_frames_) {
        view_start_ = (total_frames_ > view_frames_) ? total_frames_ - view_frames_ : 0;
    }

    params_dirty_ = true;
    request_due_ms_ = (uint32_t)(esp_timer_get_time() / 1000) + kRequestSettleMs;
}

void UISampleEditPage::sendEdit() {
    if (!has_sample_) {
        return;
    }
    // end_frame/loop_end are sent verbatim rather than as the 0 sentinel: we
    // know the real length here, so let the backend clamp against the file
    // rather than guessing what "to the end" meant.
    inter_mcu_send_sample_edit(kPreviewSlot,
                               loop_enabled_,
                               gain_db_x10_,
                               start_frame_,
                               end_frame_,
                               loop_start_,
                               loop_end_);
}

void UISampleEditPage::refreshParams() {
    char buf[40];
    const uint32_t span = view_frames_ ? view_frames_ : 1;

    struct Mark {
        uint8_t p;
        uint32_t v;
    };
    const Mark marks[4] = {{PARAM_START, start_frame_},
                           {PARAM_END, end_frame_},
                           {PARAM_LOOP_START, loop_start_},
                           {PARAM_LOOP_END, loop_end_}};
    for (const auto& m: marks) {
        if (!cards_[m.p].value) {
            continue;
        }
        formatFrames(buf, sizeof(buf), m.v, sample_rate_);
        lv_label_set_text(cards_[m.p].value, buf);
        // Position within the WHOLE sample, not the zoom window: zooming
        // should not make a marker's bar appear to jump.
        const int pct = total_frames_
                            ? static_cast<int>(std::min<uint64_t>(
                                  100, (static_cast<uint64_t>(m.v) * 100ull) / total_frames_))
                            : 0;
        lv_obj_set_width(cards_[m.p].bar, (kGaugeW * pct) / 100);
        lv_obj_set_x(cards_[m.p].knob, 16 + (kGaugeW * pct) / 100 - 4);
    }

    if (cards_[PARAM_GAIN].value) {
        snprintf(buf,
                 sizeof(buf),
                 "%s%d.%d dB",
                 gain_db_x10_ > 0 ? "+" : "",
                 gain_db_x10_ / 10,
                 (gain_db_x10_ < 0 ? -gain_db_x10_ : gain_db_x10_) % 10);
        lv_label_set_text(cards_[PARAM_GAIN].value, buf);
        const int pct = ((gain_db_x10_ + 240) * 100) / 360;  // -24..+12 dB
        lv_obj_set_width(cards_[PARAM_GAIN].bar, (kGaugeW * pct) / 100);
        lv_obj_set_x(cards_[PARAM_GAIN].knob, 16 + (kGaugeW * pct) / 100 - 4);
        lv_obj_set_style_bg_color(
            cards_[PARAM_GAIN].bar, lv_color_hex(gain_db_x10_ > 0 ? kColOrange : kColBlue), 0);
    }

    layoutParamStrip();

    // Handles are placed against the ZOOM window, which is what the waveform
    // beneath them shows. A marker outside the window parks at the edge rather
    // than disappearing, so it stays reachable.
    auto place = [&](lv_obj_t* handle, uint32_t frame, int width) {
        if (!handle) {
            return;
        }
        int64_t rel = static_cast<int64_t>(frame) - static_cast<int64_t>(view_start_);
        int64_t x = (rel * kWaveW) / static_cast<int64_t>(span);
        if (x < 0) {
            x = 0;
        }
        if (x > kWaveW - width) {
            x = kWaveW - width;
        }
        lv_obj_set_x(handle, kMargin + static_cast<int>(x));
    };
    place(marker_s_, start_frame_, 30);
    place(marker_e_, end_frame_, 30);
    place(marker_ls_, loop_start_, 34);
    place(marker_le_, loop_end_, 34);

    // Loop handles dim when looping is off: they still show where the loop
    // would be, without implying it is running.
    if (marker_ls_ && marker_le_) {
        const lv_opa_t opa = loop_enabled_ ? LV_OPA_COVER : LV_OPA_40;
        lv_obj_set_style_bg_opa(marker_ls_, opa, 0);
        lv_obj_set_style_bg_opa(marker_le_, opa, 0);
    }

    if (info_label_) {
        char sel[32], win[32], tot[32];
        formatFrames(sel, sizeof(sel), end_frame_ - start_frame_, sample_rate_);
        formatFrames(win, sizeof(win), view_frames_, sample_rate_);
        formatFrames(tot, sizeof(tot), total_frames_, sample_rate_);
        char line[220];
        snprintf(line,
                 sizeof(line),
                 "selection %s of %s  -  %lu Hz  -  window %s (1:%lu)  -  loop %s",
                 sel,
                 tot,
                 (unsigned long)sample_rate_,
                 win,
                 (unsigned long)(view_frames_ ? total_frames_ / view_frames_ : 1),
                 loop_enabled_ ? "ON" : "off");
        lv_label_set_text(info_label_, line);
    }
}

void UISampleEditPage::refreshFocusRing() {
    for (uint8_t i = 0; i < PARAM_COUNT; i++) {
        if (!cards_[i].card) {
            continue;
        }
        const bool on = (i == focus_);
        lv_obj_set_style_border_width(cards_[i].card, on ? 2 : 1, 0);
        lv_obj_set_style_border_color(cards_[i].card, lv_color_hex(on ? kColBlue : kColBorder), 0);
    }
}

void UISampleEditPage::waveChunkStatic(uint32_t offset,
                                       const int16_t* samples,
                                       uint16_t count,
                                       void* user) {
    if (!user)
        return;
    static_cast<UISampleEditPage*>(user)->handleWaveChunk(offset, samples, count);
}

// UART RX task context. Touching LVGL from here is what froze the display:
// lv_chart and lv_label are not safe outside the UI task without the port
// lock, and a chunk train from one preview request lands dozens of times.
// Copy into the buffer, raise a flag, and let serviceUi() draw.
void UISampleEditPage::handleWaveChunk(uint32_t offset, const int16_t* samples, uint16_t count) {
    if (!samples || count == 0) {
        waveform_dirty_ = true;
        return;
    }

    const uint32_t needed = offset + count;
    if (preview_buffer_.size() < kMaxPreviewPoints || needed > expected_len_) {
        // Bound the growth. A malformed or superseded reply with a large
        // offset would otherwise resize the buffer without limit from a task
        // that must not block.
        return;  // superseded or malformed reply; never grow the buffer here
    }

    std::copy(samples, samples + count, preview_buffer_.begin() + offset);
    waveform_dirty_ = true;
}

void UISampleEditPage::uiTimerCb(lv_timer_t* t) {
    auto* self = static_cast<UISampleEditPage*>(lv_timer_get_user_data(t));
    if (self) {
        self->serviceUi();
    }
}

// UI task. The only place this page touches LVGL after onEnter.
void UISampleEditPage::serviceUi() {
    if (request_due_ms_ != 0) {
        const uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
        if ((int32_t)(now - request_due_ms_) >= 0) {
            request_due_ms_ = 0;
            requestWaveform();
        }
    }
    if (params_dirty_) {
        params_dirty_ = false;
        refreshParams();
    }
    if (waveform_dirty_) {
        waveform_dirty_ = false;
        const uint32_t filled =
            std::min<uint32_t>(static_cast<uint32_t>(preview_buffer_.size()), expected_len_);
        if (waveform_ && filled > 0) {
            waveform_->setSamples(preview_buffer_.data(), static_cast<uint16_t>(filled));
        }
    }
}

void UISampleEditPage::requestWaveform() {
    if (!has_sample_) {
        refreshStatus("No sample loaded. Load via Sample Browser first.");
        return;
    }
    // Decimate so a full window always arrives as roughly kPreviewPoints,
    // whatever the zoom - otherwise zooming in would fetch the same number of
    // frames and show the same detail.
    const uint32_t span = view_frames_ ? view_frames_ : total_frames_;
    uint32_t decim = span / kPreviewPoints;
    if (decim == 0) {
        decim = 1;
    }
    expected_len_ = std::min<uint32_t>(span / decim, kMaxPreviewPoints);
    // Cleared in place - assign() would reallocate, and the UART task may be
    // writing into this buffer right now.
    std::fill(preview_buffer_.begin(), preview_buffer_.end(), 0);

    // Request the visible window, not always from zero - that is what makes
    // zooming show more detail rather than the same decimated overview.
    const esp_err_t res = inter_mcu_send_preview_req(
        kPreviewSlot, view_start_, view_start_ + span, static_cast<uint16_t>(decim));
    if (res != ESP_OK) {
        refreshStatus("Waveform request failed");
    }
}

void UISampleEditPage::refreshStatus(const char* text) {
    if (status_label_) {
        lv_label_set_text(status_label_, text);
    }
}

std::shared_ptr<UIPage> createSampleEditPage() {
    return std::make_shared<UISampleEditPage>();
}

}  // namespace wavex_ui
