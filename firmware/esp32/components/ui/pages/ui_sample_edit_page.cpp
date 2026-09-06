#include "ui/ui_sample_edit_page.h"

#include <esp_log.h>
#include <esp_timer.h>

#include "components/envelope_cache.h"
#include "components/envelope_panel.h"
#include "components/waveform_view.h"
#include "inter_mcu.h"
#include "ui/current_sample.h"
#include "ui/ui_navigator.h"
#include "ui/ui_palette.h"
#include "ui_theme.h"

#include <algorithm>
#include <cstdio>
#include <string>

namespace wavex_ui {

namespace {

// Fallback only, used when the cached SampleMetadata carried no usable
// duration. This used to be the assumed sample length, which is exactly one
// second at 48 kHz - the reason markers would not move past 1 s and zoom
// would not open out.
constexpr uint32_t kFallbackFrames = 48000;

constexpr uint32_t kMinWindow = 256;

// Shortest loop the backend will keep enabled (audio_engine.cpp
// kMinLoopFrames). Note what it does with a shorter one: it does NOT reject
// the edit, it silently clears loop_enabled - so without this the user drags a
// tight loop, sees LOOP ON, and watches it turn itself off a round trip later.
// Dragging makes that easy to hit in a way the encoder never did.
constexpr uint32_t kMinLoopFrames = 256;

// Coalescing window for drag-driven edits, ~12 sends/second while a handle is
// moving. Fast enough that an audition follows the handle, slow enough that a
// drag is not a burst on the link.
constexpr uint32_t kEditSettleMs = 80;

// Longest region fade the page offers. Past a second this stops being a fade
// on a sample and becomes an envelope, which is the instrument's job.
constexpr uint16_t kMaxFadeMs = 1000;

// Coalescing window for encoder-driven envelope requests. Long enough that a
// continuous turn produces one request rather than one per detent, short
// enough that the waveform still feels like it is tracking the knob.
constexpr uint32_t kRequestSettleMs = 150;

// A run that never completes must not wedge the page: the backend drops a
// scan when the sample it was measuring is reloaded, and says nothing.
constexpr uint32_t kRequestTimeoutMs = 3000;

// Consecutive timeouts before the page stops asking. A dropped scan is usually
// transient (the sample was being reloaded underneath it), so retrying is worth
// it; a sample the backend does not have would otherwise poll forever, and a
// status line that says so is more use than silent traffic.
constexpr uint8_t kMaxRequestRetries = 3;

// Layout, page-relative. This page is a tab body inside the Sample group, so
// what it actually gets is the content area MINUS the tab bar - 501px, not
// 557. The previous constants were budgeted against 545 with no allowance for
// the bar at all, which put the info strip's second line under the softkey
// cards; it was cut off before this change and is sized to fit here.
//
// Design turn 3c: waveform across the top, four parameter cards on a fixed
// pitch under it, the info strip last.
constexpr int kPageH = UI_CONTENT_HEIGHT - UI_TAB_BAR_HEIGHT;  // 501
constexpr int kMargin = UI_MARGIN_X;
constexpr int kWaveW = UI_SCREEN_WIDTH - 2 * kMargin;  // 1240
constexpr int kWaveY = 12;
constexpr int kWaveH = 220;
constexpr int kStripY = kWaveY + kWaveH + 16;  // 290
constexpr int kCardH = 132;
constexpr int kCardGap = 10;
constexpr int kCardW = (kWaveW - 3 * kCardGap) / 4;  // 302
constexpr int kCardPitch = kCardW + kCardGap;
// The waveform panel's content area: inside its 1 px border and 4 px
// padding, which LVGL takes off both sides. The views are built at exactly
// this size, in pixels, because a WaveformView draws one envelope column per
// pixel of its width and a percentage width could not tell it how many.
constexpr int kWavePad = 4;
constexpr int kWaveBorder = 1;
constexpr int kWaveInnerW = kWaveW - 2 * (kWavePad + kWaveBorder);  // 1230
constexpr int kWaveInnerH = kWaveH - 2 * (kWavePad + kWaveBorder);  // 210
constexpr int kSeamW = 2;
constexpr int kSpliceHalfW = (kWaveInnerW - kSeamW) / 2;  // 614

// The panel's views, in the order the sinks are attached.
constexpr uint8_t kViewContinuous = 0;
constexpr uint8_t kViewSpliceLeft = 1;
constexpr uint8_t kViewSpliceRight = 2;

constexpr int kInfoY = kStripY + kCardH + 16;  // 438
constexpr int kInfoH = kPageH - kInfoY - 12;   // 93

// Local names for the shared palette (ui/ui_palette.h). These were
// hand-copied literals that had already drifted from it and from each
// other - three different "border" greys existed across five files - so a
// theme switch reached only the surfaces that happened to be in sync.
constexpr uint32_t kColCard = palette::kColCard;
constexpr uint32_t kColBorder = palette::kColBorder;
constexpr uint32_t kColDim = palette::kColDim;
constexpr uint32_t kColBlue = palette::kColBlue;
constexpr uint32_t kColGreen = palette::kColGreen;
constexpr uint32_t kColOrange = palette::kColOrange;

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
    lv_obj_set_style_bg_color(root_, UI_COLOR_BG, 0);
    lv_obj_set_style_bg_opa(root_, LV_OPA_COVER, 0);
    lv_obj_remove_flag(root_, LV_OBJ_FLAG_SCROLLABLE);

    EnsureEnvelopeCacheInitialised();

    buildWaveformPanel(root_);
    buildParamStrip(root_);
    buildInfoStrip(root_);

    ui_timer_ = lv_timer_create(&UISampleEditPage::uiTimerCb, 50, this);

    // The panel owns the whole waveform cycle from here: it takes the chunk
    // listener, asks the cache for what each view is missing, and re-renders
    // the views as runs land. serviceUi() drives it and reads back events.
    EnvelopePanel::Config panel_cfg;
    panel_cfg.timeout_ms = kRequestTimeoutMs;
    panel_cfg.max_retries = kMaxRequestRetries;
    panel_cfg.settle_ms = kRequestSettleMs;
    EnvelopeSink* sinks[] = {waveform_.get(), splice_left_.get(), splice_right_.get()};
    panel_.attach(panel_cfg, EspEnvelopeLink(), &GetEnvelopeCache(), sinks, 3);
    panel_sample_id_ = 0;

    // The current sample: the Browser's Load, or the Sample Manager's Edit
    // softkey (track-and-patch-model.md §6 stage 0), whichever set it last.
    // Geometry comes from the backend's own cached SampleMetadata rather than
    // the Browser's listing, so any resident sample is editable regardless of
    // how it arrived.
    WaveX::Protocol::SampleMetadata m;
    has_sample_ = currentSampleId() != 0 && inter_mcu_get_sample_meta(currentSampleId(), &m);
    if (!has_sample_) {
        refreshStatus("No sample selected. Load one, or Edit from Sample Manager.");
        refreshParams();
        return;
    }

    // Opens fully zoomed out with the region spanning the whole clip, which is
    // the only starting point from which every marker is reachable.
    applyMeta(m);
    if (total_frames_ == 0) {
        total_frames_ = kFallbackFrames;
        ESP_LOGW(TAG,
                 "No duration in sample metadata; falling back to %lu frames",
                 (unsigned long)total_frames_);
    }
    zoomToFit();

    refreshStatus(m.name);
    refreshParams();
    syncWindows();
}

void UISampleEditPage::buildWaveformPanel(lv_obj_t* parent) {
    lv_obj_t* panel = box(parent, kMargin, kWaveY, kWaveW, kWaveH, palette::kColCard);
    lv_obj_set_style_border_width(panel, kWaveBorder, 0);
    lv_obj_set_style_border_color(panel, lv_color_hex(kColBorder), 0);
    lv_obj_set_style_pad_all(panel, kWavePad, 0);

    wave_panel_ = panel;
    waveform_ = std::make_unique<WaveformView>(panel, kWaveInnerW, kWaveInnerH);

    // Splice pair, built alongside the continuous view and hidden until a loop
    // marker is focused. Building both up front keeps the swap to a visibility
    // change: creating widgets on a mode switch would put an allocation and a
    // layout pass in the middle of an encoder turn.
    splice_left_ = std::make_unique<WaveformView>(panel, kSpliceHalfW, kWaveInnerH);
    lv_obj_set_pos(splice_left_->root(), 0, 0);
    splice_right_ = std::make_unique<WaveformView>(panel, kSpliceHalfW, kWaveInnerH);
    lv_obj_set_pos(splice_right_->root(), kSpliceHalfW + kSeamW, 0);

    // The seam itself, in the region-end colour: what is to its left is the
    // audio that plays last before the loop wraps.
    splice_seam_ = box(panel, kSpliceHalfW, 0, kSeamW, kWaveInnerH, kColOrange);

    // Name the view. A splice looks like an ordinary waveform with an
    // unexplained line down it unless it says otherwise, and the whole point
    // of this page's channel labelling was not to leave that ambiguous.
    splice_label_ = label(panel, 8, 4, "LOOP SEAM   end |  start", UI_FONT_MICRO, kColDim);

    for (lv_obj_t* o: {splice_left_->root(), splice_right_->root(), splice_seam_, splice_label_}) {
        lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
    }

    // Region handles: 30x26 tabs on the top edge, S green / E orange, matching
    // the marker colours the browser's preview already uses.
    marker_s_ = box(parent, kMargin, kWaveY, 30, 26, kColGreen);
    lv_obj_t* ls = label(marker_s_, 0, 0, "S", UI_FONT_SMALL, palette::kColAccentFg);
    lv_obj_center(ls);
    marker_e_ = box(parent, kMargin + kWaveW - 30, kWaveY, 30, 26, kColOrange);
    lv_obj_t* le = label(marker_e_, 0, 0, "E", UI_FONT_SMALL, palette::kColAccentFg);
    lv_obj_center(le);

    // Loop handles on the bottom edge, so they never overlap S/E even when a
    // loop sits exactly on the region bounds - which is the default.
    marker_ls_ = box(parent, kMargin, kWaveY + kWaveH - 26, 34, 26, kColBlue);
    lv_obj_t* lls = label(marker_ls_, 0, 0, "LS", UI_FONT_MICRO, palette::kColAccentFg);
    lv_obj_center(lls);
    marker_le_ = box(parent, kMargin + kWaveW - 34, kWaveY + kWaveH - 26, 34, 26, kColBlue);
    lv_obj_t* lle = label(marker_le_, 0, 0, "LE", UI_FONT_MICRO, palette::kColAccentFg);
    lv_obj_center(lle);

    // Touch-draggable (roadmap 1.5.2 item 2). The handles are 30-34 px wide,
    // which is below a comfortable touch target, so the hit area is extended
    // rather than the drawn tab made bigger - the design's geometry stays put
    // and the thing you can hit is larger than the thing you can see.
    for (lv_obj_t* handle: {marker_s_, marker_e_, marker_ls_, marker_le_}) {
        lv_obj_add_flag(handle, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_ext_click_area(handle, 12);
        lv_obj_add_event_cb(handle, &UISampleEditPage::handleEventCb, LV_EVENT_PRESSING, this);
        lv_obj_add_event_cb(handle, &UISampleEditPage::handleEventCb, LV_EVENT_RELEASED, this);
    }
}

void UISampleEditPage::buildParamStrip(lv_obj_t* parent) {
    static const char* titles[PARAM_COUNT] = {
        "START", "END", "LOOP START", "LOOP END", "GAIN", "FADE IN", "FADE OUT"};
    // Seven parameters, four card slots. The strip shows a window onto the
    // parameter list so the design's 305px card pitch survives; < Param /
    // Param > scroll it. Cramming them into the same width would shrink every
    // card below the readable-from-a-metre size the layout is built around.
    for (int i = 0; i < PARAM_COUNT; i++) {
        // The same widget as every other parameter card in the UI. This was a
        // private near-copy - label, mono value, track, fill and handle built
        // by hand - which is exactly how two "identical" cards drift apart.
        cards_[i] = valueTileCreate(parent, kMargin, kStripY, kCardW, kCardH, titles[i], nullptr);
        const int idx = i;
        valueTileSetOnAdjust(cards_[i], [this, idx](int steps) {
            focus_ = static_cast<uint8_t>(idx);
            refreshFocusRing();
            adjustFocused(steps);
        });
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
    lv_obj_set_style_radius(info, UI_RADIUS_CARD, 0);

    info_label_ = label(info, 16, 14, "", UI_FONT_SMALL, kColDim);
    status_label_ = label(info, 16, 48, "", UI_FONT_SMALL, kColGreen);
    lv_obj_set_width(status_label_, kWaveW - 32);
    lv_label_set_long_mode(status_label_, LV_LABEL_LONG_DOT);
}

void UISampleEditPage::onExit() {
    // Detach first. It releases the chunk listener before anything else, so
    // deleting the timer and the views below cannot race a chunk arriving on
    // the RX task, and it releases the cache's run in flight, which would
    // otherwise wedge the shared cache for whatever draws a waveform next.
    panel_.detach();
    if (auditioning_) {
        inter_mcu_send_sample_stop_req();
        auditioning_ = false;
    }
    if (ui_timer_) {
        lv_timer_delete(ui_timer_);
        ui_timer_ = nullptr;
    }
    waveform_.reset();
    splice_left_.reset();
    splice_right_.reset();
    if (root_) {
        lv_obj_del(root_);
        root_ = nullptr;
    }
    status_label_ = nullptr;
    info_label_ = nullptr;
    marker_s_ = nullptr;
    marker_e_ = nullptr;
    for (auto& c: cards_) {
        c = ValueTile{};
    }
}

void UISampleEditPage::onInput(const InputEvent& evt) {
    switch (evt.type) {
        // steps() is the shared encoder contract - magnitude from delta,
        // direction from the event type. This page open-coded it after
        // shipping inverted; the helper is what stops the next page repeating
        // that (roadmap 1.5.2 item 5).
        case InputType::EncoderRight:
        case InputType::EncoderUp:
        case InputType::EncoderLeft:
        case InputType::EncoderDown:
            adjustFocused(evt.steps());
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
               }};
    return keys;
}

// Preview the sample being edited, not a file on disk.
//
// This used to send MSG_SAMPLE_PLAY_INDEX_REQ with the *browser's* cursor
// position, which streamed a file straight off the card through the ring
// buffer. Two things were wrong with that: it played whatever the browser
// happened to be sitting on rather than the sample this page is editing, and
// the streaming path knows nothing about markers, loop, gain or fades - so no
// edit made here was ever audible through this button. The status line even
// admitted half of it ("range not sent - no protocol").
//
// A note-on instead runs the RAM-resident voice path, and OnNoteOn builds the
// voice from the sample's own record: start/end, loop points, gain and fades
// all apply. So the preview is what the UI says it is.
//
// kAuditionNote is the engine's default root note, so the sample plays at its
// original pitch rather than transposed.
void UISampleEditPage::toggleAudition() {
    static constexpr uint8_t kAuditionNote = 60;  // engine's kDefaultRootNote
    static constexpr uint8_t kAuditionVelocity = 100;

    if (!has_sample_) {
        refreshStatus("No sample loaded. Load via Sample Browser first.");
        return;
    }

    if (auditioning_) {
        if (inter_mcu_send_note_off_track(kAuditionNote, 0) == ESP_OK) {
            auditioning_ = false;
            refreshStatus("Stopped");
        } else {
            refreshStatus("Stop request failed");
        }
    } else {
        // Address the sample this page is editing, not whatever was loaded
        // last. Without this the preview silently followed the most recent
        // load, so opening the editor on an earlier sample previewed a
        // different one. Slot 0 to match the note-on/off below - this
        // audition path is independent of whatever slot the Voice or Play
        // page has selected.
        inter_mcu_send_sample_select(currentSampleId(), 0);
        // Push the current UI values so the voice is built from what is on
        // screen, not from whatever was last committed.
        sendEdit();
        if (inter_mcu_send_note_on_track(kAuditionNote, kAuditionVelocity, 0) == ESP_OK) {
            auditioning_ = true;
            refreshStatus("Previewing edited sample (region, loop, gain and fades applied)");
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
    if (focus_ == PARAM_FADE_IN || focus_ == PARAM_FADE_OUT) {
        // 1 ms a detent up to 20 ms, then 5 ms - a de-click lives in the first
        // few milliseconds, and a fade you can hear as a fade lives above 50,
        // so a single linear step would make one of the two useless.
        uint16_t& target = (focus_ == PARAM_FADE_IN) ? fade_in_ms_ : fade_out_ms_;
        int32_t v = target;
        for (int i = 0; i < (steps < 0 ? -steps : steps); ++i) {
            const int32_t step = (v >= 20) ? 5 : 1;
            v += (steps < 0) ? -step : step;
            if (v < 0) {
                v = 0;
            }
        }
        if (v > kMaxFadeMs) {
            v = kMaxFadeMs;
        }
        target = static_cast<uint16_t>(v);
        params_dirty_ = true;
        sendEdit();
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
}

// Adopts the backend's view. Called on entry and whenever a fresh record
// arrives, so the display always shows what the engine actually applied -
// which matters because the backend clamps and can refuse a short loop.
void UISampleEditPage::applyMeta(const WaveX::Protocol::SampleMetadata& m) {
    sample_rate_ = m.sample_rate ? m.sample_rate : 48000;
    total_frames_ = m.total_frames;
    generation_ = m.generation;
    start_frame_ = m.start_frame;
    end_frame_ = m.end_frame;
    loop_start_ = m.loop_start;
    loop_end_ = m.loop_end;
    loop_enabled_ = (m.loop_enabled != 0);
    gain_db_x10_ = m.gain_db_x10;
    fade_in_ms_ = m.fade_in_ms;
    fade_out_ms_ = m.fade_out_ms;
    if (view_frames_ == 0 || view_frames_ > total_frames_) {
        zoomToFit();
    }
    params_dirty_ = true;
}

void UISampleEditPage::sendEdit() {
    if (!has_sample_) {
        return;
    }
    // end_frame/loop_end are sent verbatim rather than as the 0 sentinel: we
    // know the real length here, so let the backend clamp against the file
    // rather than guessing what "to the end" meant.
    inter_mcu_send_sample_edit(currentSampleId(),
                               loop_enabled_,
                               gain_db_x10_,
                               start_frame_,
                               end_frame_,
                               loop_start_,
                               loop_end_,
                               fade_in_ms_,
                               fade_out_ms_);
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
        valueTileSetValue(cards_[m.p], buf);
        // Position within the WHOLE sample, not the zoom window: zooming
        // should not make a marker's bar appear to jump.
        const int pct = total_frames_
                            ? static_cast<int>(std::min<uint64_t>(
                                  100, (static_cast<uint64_t>(m.v) * 100ull) / total_frames_))
                            : 0;
        valueTileSetFill(cards_[m.p], static_cast<float>(pct) / 100.0f);
    }

    if (cards_[PARAM_GAIN].value) {
        snprintf(buf,
                 sizeof(buf),
                 "%s%d.%d dB",
                 gain_db_x10_ > 0 ? "+" : "",
                 gain_db_x10_ / 10,
                 (gain_db_x10_ < 0 ? -gain_db_x10_ : gain_db_x10_) % 10);
        valueTileSetValue(cards_[PARAM_GAIN], buf);
        const int pct = ((gain_db_x10_ + 240) * 100) / 360;  // -24..+12 dB
        valueTileSetFill(cards_[PARAM_GAIN], static_cast<float>(pct) / 100.0f);
        // Boost and cut are opposite sides of unity gain and only one of them
        // can clip. As a tone this survives focus changes; as a raw fill colour
        // it was overwritten the next time the selection moved.
        valueTileSetTone(cards_[PARAM_GAIN],
                         gain_db_x10_ > 0   ? TileTone::Positive
                         : gain_db_x10_ < 0 ? TileTone::Negative
                                            : TileTone::Neutral);
    }

    for (uint8_t p: {static_cast<uint8_t>(PARAM_FADE_IN), static_cast<uint8_t>(PARAM_FADE_OUT)}) {
        if (!cards_[p].value) {
            continue;
        }
        const uint16_t ms = (p == PARAM_FADE_IN) ? fade_in_ms_ : fade_out_ms_;
        if (ms == 0) {
            snprintf(buf, sizeof(buf), "off");
        } else {
            snprintf(buf, sizeof(buf), "%u ms", (unsigned)ms);
        }
        valueTileSetValue(cards_[p], buf);
        const int pct = (ms * 100) / kMaxFadeMs;
        valueTileSetFill(cards_[p], static_cast<float>(pct) / 100.0f);
        // Doing the de-click job, or long enough to be heard as a fade - two
        // different intentions, and the number alone does not say which.
        valueTileSetTone(cards_[p], ms <= 5 ? TileTone::Positive : TileTone::Neutral);
    }

    layoutParamStrip();

    // Turning Loop off while a loop marker is focused has to drop the seam
    // view, and that path sets params_dirty_ rather than touching focus - so
    // the mode is re-evaluated here as well as on focus changes.
    updateWaveformMode();

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

// Touch events for the four handles. UI task, like every LVGL callback here.
void UISampleEditPage::handleEventCb(lv_event_t* e) {
    auto* self = static_cast<UISampleEditPage*>(lv_event_get_user_data(e));
    if (!self) {
        return;
    }
    // current_target, not target: the handles carry a label child, and an
    // event bubbling up from it would otherwise match none of the four.
    auto* target = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
    if (lv_event_get_code(e) == LV_EVENT_RELEASED) {
        // Commit the final position immediately rather than waiting out the
        // coalescing window: letting go is an explicit "this is where I want
        // it", and an edit that lands 80 ms later feels like a bug.
        //
        // Only when one is actually owed. A non-zero deadline means the marker
        // has moved since the last send; zero means the coalescing timer
        // already flushed it, and re-sending would put a duplicate on the link
        // for every tap that did not move anything.
        if (self->edit_due_ms_ != 0) {
            self->edit_due_ms_ = 0;
            self->sendEdit();
        }
        return;
    }
    self->onHandleDrag(e, target);
}

bool UISampleEditPage::pointerFrame(lv_event_t* e, uint32_t& out_frame) const {
    if (!root_ || total_frames_ == 0) {
        return false;
    }
    lv_indev_t* indev = lv_event_get_indev(e);
    if (!indev) {
        return false;
    }
    lv_point_t point;
    lv_indev_get_point(indev, &point);

    // The pointer is in screen coordinates; the waveform starts kMargin into
    // the page root. Taking the root's real coordinates rather than assuming
    // the content area's origin keeps this correct if the chrome ever moves.
    lv_area_t root_area;
    lv_obj_get_coords(root_, &root_area);
    int32_t x = point.x - (root_area.x1 + kMargin);
    if (x < 0) {
        x = 0;
    }
    if (x > kWaveW - 1) {
        x = kWaveW - 1;
    }

    // Inverse of the mapping place() draws with, so a handle lands under the
    // finger rather than near it.
    const uint32_t span = view_frames_ ? view_frames_ : total_frames_;
    const uint64_t frame =
        static_cast<uint64_t>(view_start_) + (static_cast<uint64_t>(x) * span) / kWaveW;
    out_frame = (frame > total_frames_) ? total_frames_ : static_cast<uint32_t>(frame);
    return true;
}

void UISampleEditPage::onHandleDrag(lv_event_t* e, lv_obj_t* target) {
    if (!has_sample_ || total_frames_ == 0) {
        return;
    }
    uint32_t frame = 0;
    if (!pointerFrame(e, frame)) {
        return;
    }

    // Which handle, and the range it may occupy. Ordering is enforced here
    // rather than left to clampMarkers() afterwards, because a clamp applied
    // after the fact would drag the OTHER marker along: pushing E past S makes
    // clampMarkers() move S, so the handle you are not touching walks across
    // the screen. Bounding the dragged one instead makes it stop dead against
    // its neighbour, which is what a handle should do.
    uint32_t lower = 0;
    uint32_t upper = total_frames_;
    uint32_t* value = nullptr;
    uint8_t param = focus_;

    // A loop needs to stay long enough that the backend keeps it enabled, but
    // only when the region can actually hold one - a region shorter than the
    // minimum must not become undraggable.
    const bool enforce_loop_min =
        (end_frame_ > start_frame_) && (end_frame_ - start_frame_) >= kMinLoopFrames;
    const uint32_t loop_min = enforce_loop_min ? kMinLoopFrames : 1;

    if (target == marker_s_) {
        value = &start_frame_;
        param = PARAM_START;
        upper = end_frame_ > 0 ? end_frame_ - 1 : 0;
    } else if (target == marker_e_) {
        value = &end_frame_;
        param = PARAM_END;
        lower = start_frame_ + 1;
    } else if (target == marker_ls_) {
        value = &loop_start_;
        param = PARAM_LOOP_START;
        lower = start_frame_;
        upper = (loop_end_ > loop_min) ? loop_end_ - loop_min : start_frame_;
    } else if (target == marker_le_) {
        value = &loop_end_;
        param = PARAM_LOOP_END;
        lower = loop_start_ + loop_min;
        upper = end_frame_;
    } else {
        return;
    }

    if (upper < lower) {
        upper = lower;  // a region too small to hold the separation asked for
    }
    if (frame < lower) {
        frame = lower;
    }
    if (frame > upper) {
        frame = upper;
    }
    if (*value == frame) {
        return;  // the finger moved, the marker did not: nothing to send
    }
    *value = frame;

    // Dragging a handle is also a statement about which parameter you are
    // working on, so the encoder and the zoom anchor follow it. Without this,
    // Zoom + after a drag would jump to whatever was last focused.
    if (focus_ != param) {
        focus_ = param;
        refreshFocusRing();
    }

    clampMarkers();
    params_dirty_ = true;

    // Coalesced rather than sent per event; see kEditSettleMs. Only armed when
    // idle, so a continuous drag sends on a fixed cadence instead of pushing
    // the deadline ahead of itself and going silent until release.
    if (edit_due_ms_ == 0) {
        edit_due_ms_ = (uint32_t)(esp_timer_get_time() / 1000) + kEditSettleMs;
    }
}

void UISampleEditPage::refreshFocusRing() {
    // Focus decides whether the seam is on screen, and every focus change
    // comes through here - the encoder, the < Param / Param > keys and a
    // handle drag all call it.
    updateWaveformMode();
    // The strip has to follow too. It was laid out only with the values, so
    // Param > onto a card past the visible four put the ring on a hidden
    // card and left the strip where it was until some value changed.
    layoutParamStrip();
    for (uint8_t i = 0; i < PARAM_COUNT; i++) {
        if (!cards_[i].card) {
            continue;
        }
        valueTileSetFocus(cards_[i], i == focus_);
    }
}

uint16_t UISampleEditPage::currentSampleId() const {
    return getCurrentSampleId();
}

void UISampleEditPage::uiTimerCb(lv_timer_t* t) {
    auto* self = static_cast<UISampleEditPage*>(lv_timer_get_user_data(t));
    if (self) {
        self->serviceUi();
    }
}

// UI task. The only place this page touches LVGL after onEnter.
void UISampleEditPage::serviceUi() {
    const uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);

    // Adopt any newer record. The backend clamps, so this is how a refused
    // short loop or a narrowed region becomes visible instead of the UI
    // continuing to display a request the engine did not honour.
    if (has_sample_) {
        WaveX::Protocol::SampleMetadata m;
        if (inter_mcu_get_sample_meta(currentSampleId(), &m) && m.total_frames > 0 &&
            (m.start_frame != start_frame_ || m.end_frame != end_frame_ ||
             m.loop_start != loop_start_ || m.loop_end != loop_end_ ||
             (m.loop_enabled != 0) != loop_enabled_ || m.gain_db_x10 != gain_db_x10_ ||
             m.fade_in_ms != fade_in_ms_ || m.fade_out_ms != fade_out_ms_)) {
            applyMeta(m);
        }
    }
    if (params_dirty_) {
        params_dirty_ = false;
        refreshParams();
    }
    // A drag in progress owes the backend an edit.
    if (edit_due_ms_ != 0 && (int32_t)(now - edit_due_ms_) >= 0) {
        edit_due_ms_ = 0;
        sendEdit();
    }
    // Tell the panel where every view now is - a no-op when nothing moved -
    // then let it file, ask and draw. It coalesces a moving window itself;
    // this page only words the outcomes.
    syncWindows();
    switch (panel_.service(now)) {
        case EnvelopePanel::Event::Drawn:
            if (wave_status_shown_) {
                // The trace is back; the line about it not being is not.
                wave_status_shown_ = false;
                WaveX::Protocol::SampleMetadata m;
                if (inter_mcu_get_sample_meta(currentSampleId(), &m)) {
                    refreshStatus(m.name);
                }
            }
            break;
        case EnvelopePanel::Event::Retrying:
            wave_status_shown_ = true;
            refreshStatus("Waveform request timed out - retrying");
            break;
        case EnvelopePanel::Event::SendFailed:
            wave_status_shown_ = true;
            refreshStatus("Waveform request refused by the link - retrying");
            break;
        case EnvelopePanel::Event::GaveUp:
            wave_status_shown_ = true;
            refreshStatus("Waveform unavailable - reopen the page to retry");
            break;
        case EnvelopePanel::Event::None:
            break;
    }
}

// UI task only. Hands the panel the current sample and the frames each view
// shows. Called every tick and from onEnter: every input is cheap on the
// panel's side (an unchanged window costs a compare), so this does not need
// to know which of zoom, marker, focus or loop state moved - which is exactly
// the bookkeeping the previous request_due_ms_ scattered across five methods.
void UISampleEditPage::syncWindows() {
    if (!panel_.attached()) {
        return;
    }
    const uint16_t sample_id = currentSampleId();
    if (!has_sample_ || sample_id == 0 || total_frames_ == 0) {
        if (panel_.hasSample()) {
            panel_.clearSample();
        }
        panel_sample_id_ = 0;
        return;
    }
    if (sample_id != panel_sample_id_ || generation_ != panel_generation_ ||
        total_frames_ != panel_total_frames_) {
        panel_sample_id_ = sample_id;
        panel_generation_ = generation_;
        panel_total_frames_ = total_frames_;
        panel_.setSample(sample_id, generation_, total_frames_);
    }

    const uint32_t span = view_frames_ ? view_frames_ : total_frames_;
    panel_.setWindow(kViewContinuous, view_start_, view_start_ + span);

    // Left: the audio that plays last before the wrap. Right: what it wraps
    // to. Each half is its own view at its own width, so each gets the cache
    // tier its 614 px deserve rather than a slice of the continuous view's.
    const bool splice = spliceActive();
    const uint32_t half = splice ? spliceHalfSpan() : 0;
    if (half) {
        panel_.setWindow(kViewSpliceLeft, loop_end_ - half, loop_end_);
        panel_.setWindow(kViewSpliceRight, loop_start_, loop_start_ + half);
    }
    panel_.enableView(kViewContinuous, !splice);
    panel_.enableView(kViewSpliceLeft, half != 0);
    panel_.enableView(kViewSpliceRight, half != 0);
}

bool UISampleEditPage::spliceActive() const {
    // "When editing a loop, show the seam" - so the view follows the focused
    // parameter rather than a softkey. Both softkey rows are full, and a mode
    // you have to remember to turn on is a mode that does not get used; this
    // way focusing LS or LE is the gesture.
    return has_sample_ && loop_enabled_ && (focus_ == PARAM_LOOP_START || focus_ == PARAM_LOOP_END);
}

uint32_t UISampleEditPage::spliceHalfSpan() const {
    if (total_frames_ == 0 || loop_end_ <= loop_start_) {
        return 0;
    }
    const uint32_t span = view_frames_ ? view_frames_ : total_frames_;
    uint32_t half = span / 2;

    // Both halves must cover the SAME number of frames, or the two waveforms
    // are at different scales and cannot be aligned by eye - which is the only
    // thing this view is for. So the span shrinks to whatever both sides can
    // supply rather than each side showing as much as it happens to have.
    half = std::min(half, loop_end_);
    half = std::min(half, total_frames_ - loop_start_);
    return half ? half : 1;
}

void UISampleEditPage::updateWaveformMode() {
    if (!waveform_ || !splice_left_ || !splice_right_) {
        return;
    }
    const bool splice = spliceActive();
    if (splice == splice_shown_) {
        return;  // called on every focus change; only act on a real switch
    }
    splice_shown_ = splice;

    const auto set_hidden = [](lv_obj_t* o, bool hidden) {
        if (!o) {
            return;
        }
        if (hidden) {
            lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_remove_flag(o, LV_OBJ_FLAG_HIDDEN);
        }
    };

    set_hidden(waveform_->root(), splice);
    set_hidden(splice_left_->root(), !splice);
    set_hidden(splice_right_->root(), !splice);
    set_hidden(splice_seam_, !splice);
    set_hidden(splice_label_, !splice);

    // The region handles describe positions in the continuous view and mean
    // nothing against a seam, so they go with it rather than sitting over a
    // view whose x axis they do not share.
    for (lv_obj_t* handle: {marker_s_, marker_e_, marker_ls_, marker_le_}) {
        set_hidden(handle, splice);
    }
    // What the views show follows on the next tick: syncWindows() enables
    // the pair that just appeared, which the panel redraws from the cache at
    // once and requests through its settle - so flicking across the loop
    // params does not queue a request per step.
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
