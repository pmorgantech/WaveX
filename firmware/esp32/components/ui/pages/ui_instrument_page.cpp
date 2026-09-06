// WaveX Instrument editor
#include "ui/ui_instrument_page.h"

#include <esp_log.h>

#include "../styles/ui_theme.h"
#include "inter_mcu.h"
#include "ui/current_track.h"
#include "ui/ui_navigator.h"
#include "ui/ui_palette.h"
#include "ui/ui_sample_browser.h"
#include "ui/ui_tab_group.h"

#include <cstdio>
#include <cstring>

namespace wavex_ui {

using namespace wavex_ui::palette;

namespace {
static const char* TAG = "UI_INSTRUMENT";

// Not in the shared palette: "this control cannot be driven yet" is a state
// only this page and the softkey bar have, and it is not part of the card /
// tab design vocabulary the palette describes.
// Local names for the shared palette (ui/ui_palette.h). These were
// hand-copied literals that had already drifted from it and from each
// other - three different "border" greys existed across five files - so a
// theme switch reached only the surfaces that happened to be in sync.
constexpr uint32_t kColInert = palette::kColDimmer;

// 64 detents end to end: fine enough to sound continuous, coarse enough to
// cross the range without grinding. Matches the Play page's feel.
constexpr int kParamStep = 65535 / 64;

// Tab bar labels, in the same order as UIInstrumentPage::Stage. Kept short because
// the bar divides evenly - one long label shrinks every other tab's target.
const char* const kStageNames[] = {"Sample", "Env", "Amp", "Filter", "Mod"};

// Content geometry, from design turns 2c (Env) and 2d (Filter). Positions are
// relative to the tab body, which the navigator has already inset by the
// header, the shift rule and the softkey bar.
//
// The 84px name/status strip this page used to carry is gone: the navigator's
// header now holds that context (contextLine()), which is where it belongs -
// it was the same information on every stage, redrawn inside the page.
constexpr int kTabBarH = 56;
constexpr int kBodyH = UI_CONTENT_HEIGHT - kTabBarH;  // 501
constexpr int kBodyPadTop = 12;
constexpr int kPaneH = 466;

// Env: a 2x2 dial grid on the left, the envelope drawn on the right.
constexpr int kDialGridW = 700;
constexpr int kDialGap = 10;
constexpr int kDialW = (kDialGridW - kDialGap) / 2;                     // 345
constexpr int kDialH = (kPaneH - kDialGap) / 2;                         // 228
constexpr int kEnvCurveX = UI_MARGIN_X + kDialGridW + 16;               // 736
constexpr int kEnvCurveW = UI_SCREEN_WIDTH - UI_MARGIN_X - kEnvCurveX;  // 524

// Filter: response curve across the top, three tiles under it.
constexpr int kFilterCurveH = 270;
constexpr int kFilterTileY = kBodyPadTop + kFilterCurveH + 12;
constexpr int kFilterTileH = kPaneH - kFilterCurveH - 12;  // 184

// Stages that are neither: one row of tiles, centred in the body. Sample has
// five controls and Amp two, so the row cannot fill 501px without stretching
// tiles into panels; centring reads as deliberate where top-alignment leaves
// the page looking truncated.
constexpr int kRowTileH = 260;
constexpr int kContentW = UI_SCREEN_WIDTH - 2 * UI_MARGIN_X;  // 1240

// Curve pane internals - the plot area inside the titled card.
constexpr int kPanePadX = 20;
constexpr int kPanePadTop = 52;
constexpr int kPanePadBottom = 20;

}  // namespace

int UIInstrumentPage::paramsForStage(Stage s, Param* out, int max) const {
    int n = 0;
    auto add = [&](const char* label, uint8_t wire, uint16_t value, const char* unit) {
        if (n < max) {
            out[n++] = Param{label, wire, value, s, unit};
        }
    };

    switch (s) {
        case Stage::Sample:
            // SAMPLE and TRACK are not MSG_CONTROL_CHANGE destinations -
            // stepParam() special-cases both. SAMPLE cycles which resident
            // sample this Instrument addresses; TRACK is the shared selected
            // Track (MIDI channel) that sample is bound to for note-on playback.
            // Both ride MSG_SAMPLE_SELECT (roadmap Phase 2.5 item 1, "retire
            // the fallback"). PITCH/PAN/GAIN are live: the engine applies all
            // three to sounding voices.
            add("SAMPLE", kParamSample, 0, "");
            add("TRACK", kParamTrack, 0, "");
            // Units are "%" of the wire range, not semitones or dB. The
            // engine publishes no mapping from a 0..65535 control value to a
            // physical unit, so printing "semi" beside a percentage - which
            // this line used to do - claimed a calibration that does not
            // exist. When the engine exposes ranges, these become real units.
            add("PITCH", WaveX::Protocol::PARAM_PITCH, 32768, "%");
            add("PAN", WaveX::Protocol::PARAM_PAN, 32768, "%");
            add("GAIN", WaveX::Protocol::PARAM_VOLUME, 52428, "%");
            break;
        case Stage::Envelopes:
            add("ATTACK", WaveX::Protocol::PARAM_ENVELOPE_ATTACK, 0, "");
            add("DECAY", WaveX::Protocol::PARAM_ENVELOPE_DECAY, 1638, "");
            add("SUSTAIN", WaveX::Protocol::PARAM_ENVELOPE_SUSTAIN, 52428, "");
            add("RELEASE", WaveX::Protocol::PARAM_ENVELOPE_RELEASE, 3277, "");
            break;
        case Stage::Amp:
            // The amp stage is the envelope's destination. Level is the same
            // wire parameter as the sample stage's gain by design - there is
            // one output gain, and showing it twice under two names would
            // imply two controls that fight.
            add("LEVEL", WaveX::Protocol::PARAM_VOLUME, 52428, "%");
            add("ENV->AMP", kParamNone, 65535, "");
            break;
        case Stage::Filter:
            add("CUTOFF", WaveX::Protocol::PARAM_FILTER_CUTOFF, 65535, "%");
            add("RES", WaveX::Protocol::PARAM_FILTER_RESONANCE, 0, "%");
            add("ENV->FLT", kParamNone, 0, "");
            break;
        case Stage::Mod:
            // Inert, and labelled so. Nothing on the wire carries a modulation
            // source, destination or depth yet; drawing a matrix that silently
            // does nothing is the failure this codebase has already had once.
            add("SOURCE", kParamNone, 0, "");
            add("DEST", kParamNone, 0, "");
            add("DEPTH", kParamNone, 0, "");
            break;
        default:
            break;
    }

    // Overlay what the user has actually set. Done here rather than in each
    // add() so the table above stays a plain description of the chain.
    if (values_seeded_) {
        const int si = static_cast<int>(s);
        for (int i = 0; i < n; ++i) {
            out[i].value = stage_values_[si][i];
        }
    }
    return n;
}

// Captures the defaults from the chain description exactly once, so the page
// and the engine start from the same numbers.
void UIInstrumentPage::seedValues() {
    for (int s = 0; s < kStageCount; ++s) {
        Param params[kMaxParams];
        const int n = paramsForStage(static_cast<Stage>(s), params, kMaxParams);
        for (int i = 0; i < n; ++i) {
            stage_values_[s][i] = params[i].value;
        }
    }
    values_seeded_ = true;
}

// Which Track (0..15) the Sample tab's TRACK control currently targets.
// Shared with Play, Sample Manager and the Browser's SFZ load target
// (current_track.h) rather than kept in stage_values_ like the Instrument params:
// a Track selection that only this page knew about was one of the reasons
// "which Track?" had a different answer on every page.
uint8_t UIInstrumentPage::currentTrack() const {
    return getCurrentTrack();
}

// Steps sample_id_ to the next/previous resident sample (wrapping) and
// re-binds it to the selected Track - the Sample tab's "which sample this
// Instrument plays" control (roadmap Phase 2.5 item 1, "retire the
// fallback"). There is no "list of loaded ids" query, so this probes
// inter_mcu_get_sample_meta() over the same id range the Sample Manager
// page's list does, rather than inventing a second source of truth for it.
void UIInstrumentPage::cycleSample(int direction) {
    constexpr int32_t kMaxProbeId = 64;
    for (int32_t step = 1; step <= kMaxProbeId; ++step) {
        int32_t candidate = static_cast<int32_t>(sample_id_) + direction * step;
        // Wrap into 1..kMaxProbeId (0 is reserved for "no sample").
        candidate = ((candidate - 1) % kMaxProbeId + kMaxProbeId) % kMaxProbeId + 1;
        WaveX::Protocol::SampleMetadata m;
        if (inter_mcu_get_sample_meta(static_cast<uint16_t>(candidate), &m)) {
            sample_id_ = static_cast<uint16_t>(candidate);
            inter_mcu_send_sample_select(sample_id_, currentTrack());
            refreshHeader();
            refreshParams();
            return;
        }
    }
    refreshStatus("No other samples resident - load one from Browse");
}

void UIInstrumentPage::onEnter(lv_obj_t* parent) {
    lv_obj_clean(parent);
    stage_ = 0;
    param_ = 0;
    editing_ = false;
    for (auto& b: stage_built_) {
        b = false;
    }
    for (int s = 0; s < kStageCount; ++s) {
        for (int i = 0; i < kMaxParams; ++i) {
            tiles_[s][i] = ValueTile{};
        }
    }
    for (auto& d: dials_) {
        d = Dial{};
    }
    env_curve_ = nullptr;
    filter_curve_ = nullptr;

    root_ = lv_obj_create(parent);
    lv_obj_set_size(root_, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(root_, lv_color_hex(kColBg), LV_PART_MAIN);
    lv_obj_set_style_border_width(root_, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(root_, 0, LV_PART_MAIN);
    lv_obj_remove_flag(root_, LV_OBJ_FLAG_SCROLLABLE);

    // No in-page strip: the instrument name, Track and sample now ride the
    // navigator's header via contextLine(), so the tabview gets the full
    // content area rather than 84px less.
    lv_obj_t* tab_host = lv_obj_create(root_);
    lv_obj_set_size(tab_host, lv_pct(100), lv_pct(100));
    lv_obj_set_pos(tab_host, 0, 0);
    lv_obj_set_style_bg_color(tab_host, lv_color_hex(kColBg), LV_PART_MAIN);
    lv_obj_set_style_border_width(tab_host, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(tab_host, 0, LV_PART_MAIN);
    lv_obj_remove_flag(tab_host, LV_OBJ_FLAG_SCROLLABLE);

    tabview_ = tabGroupCreate(tab_host);
    for (int s = 0; s < kStageCount; ++s) {
        tab_body_[s] = tabGroupAddTab(tabview_, kStageNames[s]);
    }
    lv_obj_add_event_cb(tabview_, &UIInstrumentPage::tabChangedCb, LV_EVENT_VALUE_CHANGED, this);

    // The Instrument edits whatever sample is selected. Asking for metadata means
    // the header can name it rather than showing a bare id.
    seedValues();

    sample_id_ = 0;
    if (auto* state = getSampleBrowserState()) {
        sample_id_ = state->last_load_sample_id;
    }
    if (sample_id_ != 0) {
        inter_mcu_request_sample_meta(sample_id_);
        inter_mcu_send_sample_select(sample_id_, currentTrack());
    }

    // Only the first tab's widgets exist after this; the rest are built when
    // first shown.
    buildStageRows(stage_);
    refreshHeader();
    refreshParams();
}

void UIInstrumentPage::onExit() {
    if (root_) {
        lv_obj_del(root_);
        root_ = nullptr;
        tabview_ = nullptr;
        env_curve_ = nullptr;
        filter_curve_ = nullptr;
        // The widget handles point into the tree just deleted; clearing them
        // is what stops refreshParams() writing styles into freed objects if
        // it runs before the page is entered again.
        for (auto& d: dials_) {
            d = Dial{};
        }
        for (int s = 0; s < kStageCount; ++s) {
            tab_body_[s] = nullptr;
            stage_built_[s] = false;
            for (int i = 0; i < kMaxParams; ++i) {
                tiles_[s][i] = ValueTile{};
            }
        }
    }
}

// Builds one tab's parameter rows. Called on first display of that tab, not up
// front: entering the page then costs three rows rather than seventeen, and
// page entry is what this UI pays for (docs/backlog.md).
// A titled card with a plot area inside it, shared by the envelope and filter
// curves. Returns the lv_line; the caller owns updating its points.
lv_obj_t* UIInstrumentPage::buildCurvePane(lv_obj_t* parent,
                                           int x,
                                           int y,
                                           int w,
                                           int h,
                                           const char* title,
                                           const char* right,
                                           lv_point_precise_t* pts,
                                           int count) {
    lv_obj_t* card = lv_obj_create(parent);
    lv_obj_remove_style_all(card);
    lv_obj_set_pos(card, x, y);
    lv_obj_set_size(card, w, h);
    lv_obj_set_style_bg_color(card, UI_COLOR_CARD, 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(card, UI_RADIUS_CARD, 0);
    lv_obj_set_style_border_width(card, UI_BORDER_WIDTH, 0);
    lv_obj_set_style_border_color(card, UI_COLOR_LINE, 0);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* t = lv_label_create(card);
    lv_label_set_text(t, title);
    lv_obj_set_style_text_font(t, UI_FONT_SMALL, 0);
    lv_obj_set_style_text_color(t, UI_COLOR_DIM, 0);
    lv_obj_set_style_text_letter_space(t, 1, 0);
    lv_obj_set_pos(t, kPanePadX, 18);

    if (right && right[0]) {
        lv_obj_t* r = lv_label_create(card);
        lv_label_set_text(r, right);
        lv_obj_set_style_text_font(r, UI_FONT_MONO_MICRO, 0);
        lv_obj_set_style_text_color(r, UI_COLOR_DIM, 0);
        lv_obj_align(r, LV_ALIGN_TOP_RIGHT, -kPanePadX, 20);
    }

    // The plot well. A flat inset rather than the grid the design draws over
    // it: a 4x4 grid is 8 more objects per pane for decoration, and this panel
    // flushes in 20-line strips.
    lv_obj_t* well = lv_obj_create(card);
    lv_obj_remove_style_all(well);
    lv_obj_set_pos(well, kPanePadX, kPanePadTop);
    lv_obj_set_size(well, w - 2 * kPanePadX, h - kPanePadTop - kPanePadBottom);
    lv_obj_set_style_bg_color(well, UI_COLOR_CARD_ALT, 0);
    lv_obj_set_style_bg_opa(well, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(well, UI_RADIUS_BADGE, 0);
    lv_obj_remove_flag(well, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* line = lv_line_create(well);
    lv_obj_set_pos(line, 0, 0);
    lv_obj_set_style_line_color(line, UI_COLOR_ACCENT, 0);
    lv_obj_set_style_line_width(line, 4, 0);
    lv_obj_set_style_line_rounded(line, true, 0);
    lv_line_set_points(line, pts, static_cast<uint32_t>(count));
    return line;
}

void UIInstrumentPage::buildStageRows(int stage) {
    if (stage < 0 || stage >= kStageCount || stage_built_[stage] || !tab_body_[stage]) {
        return;
    }
    lv_obj_t* body = tab_body_[stage];
    lv_obj_set_style_pad_all(body, 0, 0);
    lv_obj_remove_flag(body, LV_OBJ_FLAG_SCROLLABLE);

    Param params[kMaxParams];
    const int n = paramsForStage(static_cast<Stage>(stage), params, kMaxParams);

    if (stage == static_cast<int>(Stage::Envelopes)) {
        // Four dials, 2x2, with the envelope they describe drawn beside them.
        for (int i = 0; i < n && i < 4; ++i) {
            const int col = i % 2;
            const int row = i / 2;
            dials_[i] = dialCreate(body,
                                   UI_MARGIN_X + col * (kDialW + kDialGap),
                                   kBodyPadTop + row * (kDialH + kDialGap),
                                   kDialW,
                                   kDialH,
                                   params[i].label);
        }
        env_curve_ = buildCurvePane(body,
                                    kEnvCurveX,
                                    kBodyPadTop,
                                    kEnvCurveW,
                                    kPaneH,
                                    "AMP ENVELOPE",
                                    "-> Amp",
                                    env_pts_,
                                    kEnvCurvePoints);
    } else if (stage == static_cast<int>(Stage::Filter)) {
        filter_curve_ = buildCurvePane(body,
                                       UI_MARGIN_X,
                                       kBodyPadTop,
                                       kContentW,
                                       kFilterCurveH,
                                       "LOW-PASS RESPONSE",
                                       "20 Hz - 20 kHz",
                                       filter_pts_,
                                       kFilterCurvePoints);
        const int tw = (kContentW - 2 * kDialGap) / 3;
        for (int i = 0; i < n && i < 3; ++i) {
            tiles_[stage][i] = valueTileCreate(body,
                                               UI_MARGIN_X + i * (tw + kDialGap),
                                               kFilterTileY,
                                               tw,
                                               kFilterTileH,
                                               params[i].label,
                                               params[i].unit);
        }
    } else {
        // One row of tiles, divided evenly. Sample has five, Amp two, Mod
        // three; an even division keeps every stage on the same baseline
        // rather than giving each its own bespoke grid.
        const int cols = n > 0 ? n : 1;
        const int tw = (kContentW - (cols - 1) * kDialGap) / cols;
        const int ty = (kBodyH - kRowTileH) / 2;
        for (int i = 0; i < n; ++i) {
            tiles_[stage][i] = valueTileCreate(body,
                                               UI_MARGIN_X + i * (tw + kDialGap),
                                               ty,
                                               tw,
                                               kRowTileH,
                                               params[i].label,
                                               params[i].unit);
        }
    }

    stage_built_[stage] = true;
}

// The envelope as five points: silence, attack peak, decay to sustain, the
// sustain plateau, and release back to silence. The plateau is given a fixed
// share of the width rather than a duration - sustain is a level, not a time,
// so there is nothing truthful to scale its width by.
void UIInstrumentPage::refreshEnvCurve() {
    if (!env_curve_ || !lv_obj_is_valid(env_curve_)) {
        return;
    }
    Param params[kMaxParams];
    const int n = paramsForStage(Stage::Envelopes, params, kMaxParams);
    if (n < 4) {
        return;
    }
    const int32_t w = lv_obj_get_width(lv_obj_get_parent(env_curve_));
    const int32_t h = lv_obj_get_height(lv_obj_get_parent(env_curve_));
    if (w <= 0 || h <= 0) {
        return;
    }
    const int32_t pad = 6;
    const int32_t top = pad;
    const int32_t bottom = h - pad;

    const float a = static_cast<float>(params[0].value) / 65535.0f;
    const float d = static_cast<float>(params[1].value) / 65535.0f;
    const float sus = static_cast<float>(params[2].value) / 65535.0f;
    const float r = static_cast<float>(params[3].value) / 65535.0f;

    // A, D and R share 70% of the width in proportion; the sustain plateau
    // takes the remaining 30%. Scaling them against each other is what makes
    // "attack longer than decay" visible at a glance.
    const float span = a + d + r;
    const float unit = (span > 0.001f) ? (0.70f * static_cast<float>(w)) / span : 0.0f;
    const int32_t xa = static_cast<int32_t>(a * unit);
    const int32_t xd = xa + static_cast<int32_t>(d * unit);
    const int32_t xs = xd + static_cast<int32_t>(0.30f * static_cast<float>(w));
    const int32_t ys = bottom - static_cast<int32_t>(sus * static_cast<float>(bottom - top));

    env_pts_[0] = {0, bottom};
    env_pts_[1] = {xa, top};
    env_pts_[2] = {xd, ys};
    env_pts_[3] = {xs, ys};
    env_pts_[4] = {w > xs ? w : xs, bottom};
    lv_line_set_points(env_curve_, env_pts_, kEnvCurvePoints);
}

// A one-pole low-pass magnitude response with a resonant peak at the corner.
// Not the engine's actual filter transfer function - the engine does not
// publish one - so this is a shape that moves correctly with the two controls,
// which is what the pane is for. It must not be read as a measurement.
void UIInstrumentPage::refreshFilterCurve() {
    if (!filter_curve_ || !lv_obj_is_valid(filter_curve_)) {
        return;
    }
    Param params[kMaxParams];
    const int n = paramsForStage(Stage::Filter, params, kMaxParams);
    if (n < 2) {
        return;
    }
    const int32_t w = lv_obj_get_width(lv_obj_get_parent(filter_curve_));
    const int32_t h = lv_obj_get_height(lv_obj_get_parent(filter_curve_));
    if (w <= 0 || h <= 0) {
        return;
    }
    const float cutoff = static_cast<float>(params[0].value) / 65535.0f;
    const float res = static_cast<float>(params[1].value) / 65535.0f;
    const float flat = static_cast<float>(h) * 0.42f;

    for (int i = 0; i < kFilterCurvePoints; ++i) {
        const float t = static_cast<float>(i) / static_cast<float>(kFilterCurvePoints - 1);
        const float x = t - cutoff;  // octaves-ish either side of the corner
        float y;
        if (x <= 0.0f) {
            // Below the corner: flat, lifted by the resonant peak as it nears.
            const float bump = res * 0.9f * (1.0f - (x * x) * 90.0f);
            y = flat - flat * (bump > 0.0f ? bump : 0.0f);
        } else {
            // Above it: roll off, steeply enough to read as a filter.
            const float fall = x * 3.2f;
            y = flat + (static_cast<float>(h) - flat) * (fall < 1.0f ? fall : 1.0f);
        }
        if (y < 2.0f) {
            y = 2.0f;
        }
        if (y > static_cast<float>(h) - 2.0f) {
            y = static_cast<float>(h) - 2.0f;
        }
        filter_pts_[i] = {static_cast<int32_t>(t * static_cast<float>(w)), static_cast<int32_t>(y)};
    }
    lv_line_set_points(filter_curve_, filter_pts_, kFilterCurvePoints);
}

void UIInstrumentPage::tabChangedCb(lv_event_t* e) {
    auto* self = static_cast<UIInstrumentPage*>(lv_event_get_user_data(e));
    if (!self || !self->tabview_) {
        return;
    }
    self->selectStage(static_cast<int>(lv_tabview_get_tab_active(self->tabview_)));
}

// Single entry point for "the visible stage is now this one", whether the user
// tapped the bar or the softkeys/encoder drove it. moveStage() sets the active
// tab, which re-enters here through the event callback, so this has to be a
// no-op when the stage has not actually changed.
void UIInstrumentPage::selectStage(int stage) {
    if (stage < 0 || stage >= kStageCount || stage == stage_) {
        return;
    }
    stage_ = stage;
    param_ = 0;
    buildStageRows(stage_);
    // Sample metadata arrives asynchronously, so the header may only be able to
    // name the sample some time after onEnter() first drew it.
    refreshHeader();
    refreshParams();
    UINavigator::instance().refreshSoftkeys();
    ESP_LOGI(TAG, "Instrument -> %s", kStageNames[stage_]);
}

// Builds the string the navigator draws beside the page title. Held in a
// member because UIPage::contextLine() hands back a pointer the navigator
// copies immediately.
void UIInstrumentPage::refreshHeader() {
    WaveX::Protocol::SampleMetadata m;
    const unsigned track = trackDisplayNumber(currentTrack());
    if (sample_id_ != 0 && inter_mcu_get_sample_meta(sample_id_, &m)) {
        snprintf(context_line_,
                 sizeof(context_line_),
                 "%s / Track %u / %.24s",
                 instrument_name_,
                 track,
                 m.name);
    } else if (sample_id_ != 0) {
        snprintf(context_line_,
                 sizeof(context_line_),
                 "%s / Track %u / sample %u",
                 instrument_name_,
                 track,
                 (unsigned)sample_id_);
    } else {
        snprintf(context_line_,
                 sizeof(context_line_),
                 "%s / Track %u / no sample",
                 instrument_name_,
                 track);
    }
    // A status message displaces the sample for as long as it is set: it is
    // the more recent thing the user did, and the header is the only place
    // left to say it now the in-page strip is gone.
    if (status_[0]) {
        snprintf(context_line_ + strlen(context_line_),
                 sizeof(context_line_) - strlen(context_line_),
                 " / %s",
                 status_);
    }
    UINavigator::instance().refreshContext();
}

void UIInstrumentPage::refreshParams() {
    Param params[kMaxParams];
    const int n = paramsForStage(static_cast<Stage>(stage_), params, kMaxParams);
    if (param_ >= n) {
        param_ = n > 0 ? n - 1 : 0;
    }
    const bool env = (stage_ == static_cast<int>(Stage::Envelopes));

    for (int i = 0; i < n; ++i) {
        const Param& p = params[i];
        const bool inert = (p.wire_param == kParamNone);
        const bool focused = (i == param_);
        const float frac = static_cast<float>(p.value) / 65535.0f;

        if (env) {
            if (!dials_[i].card) {
                continue;
            }
            // Envelope times are shown as a percentage of range, not in ms:
            // the engine does not tell us what its range maps to in seconds,
            // and printing an invented "12 ms" would be a measurement claim.
            char value[16];
            snprintf(value, sizeof(value), "%d%%", static_cast<int>(frac * 100.0f + 0.5f));
            dialSetValue(dials_[i], frac, value, i == 2 ? "level" : "time");
            dialSetFocus(dials_[i], focused);
            continue;
        }

        ValueTile& tile = tiles_[stage_][i];
        if (!tile.card) {
            continue;
        }
        if (inert) {
            // Set once: valueTileSetUnwired() adds a reason label each call.
            if (!tile.note) {
                valueTileSetUnwired(tile, "protocol carries no mod routing yet");
            }
            continue;
        }

        char value[40];
        if (p.wire_param == kParamSample) {
            WaveX::Protocol::SampleMetadata m;
            if (sample_id_ == 0) {
                snprintf(value, sizeof(value), "none");
            } else if (inter_mcu_get_sample_meta(sample_id_, &m)) {
                snprintf(value, sizeof(value), "%.18s", m.name);
            } else {
                snprintf(value, sizeof(value), "%u", (unsigned)sample_id_);
            }
            valueTileSetValue(tile, value, true);
            valueTileSetFill(tile, 0.0f);
        } else if (p.wire_param == kParamTrack) {
            snprintf(value, sizeof(value), "%u", trackDisplayNumber(getCurrentTrack()));
            valueTileSetValue(tile, value);
            valueTileSetFill(tile, 0.0f);
        } else {
            snprintf(value, sizeof(value), "%d", static_cast<int>(frac * 100.0f + 0.5f));
            valueTileSetValue(tile, value);
            valueTileSetFill(tile, frac);
        }
        valueTileSetFocus(tile, focused);
    }

    if (env) {
        refreshEnvCurve();
    } else if (stage_ == static_cast<int>(Stage::Filter)) {
        refreshFilterCurve();
    }
}

void UIInstrumentPage::refreshStatus(const char* text) {
    snprintf(status_, sizeof(status_), "%s", text ? text : "");
    refreshHeader();
}

void UIInstrumentPage::sendParam(const Param& p) {
    if (p.wire_param == kParamNone) {
        refreshStatus("Not wired: the protocol carries no modulation routing yet");
        return;
    }
    if (p.wire_param == kParamSample || p.wire_param == kParamTrack) {
        // Not a CC destination - stepParam() is what actually sends
        // MSG_SAMPLE_SELECT for these two. sendParam() is also called from
        // Init's "resend every wired parameter" loop, which must not forward
        // a sentinel as a bogus PARAM_* id.
        return;
    }
    if (inter_mcu_send_control_change(p.wire_param, 0, p.value) != ESP_OK) {
        refreshStatus("Send failed - link busy?");
    }
}

void UIInstrumentPage::stepParam(int steps) {
    Param params[kMaxParams];
    const int n = paramsForStage(static_cast<Stage>(stage_), params, kMaxParams);
    if (param_ < 0 || param_ >= n) {
        return;
    }
    Param& p = params[param_];
    if (p.wire_param == kParamNone) {
        refreshStatus("Not wired: the protocol carries no modulation routing yet");
        return;
    }
    if (p.wire_param == kParamSample) {
        cycleSample(steps > 0 ? 1 : -1);
        return;
    }
    if (p.wire_param == kParamTrack) {
        int32_t next = static_cast<int32_t>(getCurrentTrack()) + steps;
        next = next < 0 ? 0 : (next > 15 ? 15 : next);
        setCurrentTrack(static_cast<uint8_t>(next));
        // Re-bind whatever sample this page is tracking to the new Track, so
        // moving TRACK genuinely changes which channel's note-on plays it
        // rather than just relabelling a number nothing reads.
        if (sample_id_ != 0 &&
            inter_mcu_send_sample_select(sample_id_, getCurrentTrack()) != ESP_OK) {
            refreshStatus("Send failed - link busy?");
        }
        refreshHeader();
        refreshParams();
        return;
    }

    const int32_t delta = static_cast<int32_t>(steps) * kParamStep;
    int32_t next = static_cast<int32_t>(p.value) + delta;
    next = next < 0 ? 0 : (next > 65535 ? 65535 : next);
    p.value = static_cast<uint16_t>(next);

    // Values live in the table this function rebuilt, so persist them back into
    // the page's own copy before sending. Kept in one place rather than a
    // parallel array so the table stays the single description of the chain.
    stage_values_[stage_][param_] = p.value;

    sendParam(p);
    refreshParams();
}

// Softkey/encoder stage movement drives the tab bar rather than a second piece
// of state, so the bar always shows where the focus actually is.
void UIInstrumentPage::moveStage(int delta) {
    const int next = (stage_ + delta + kStageCount) % kStageCount;
    if (tabview_ && lv_obj_is_valid(tabview_)) {
        lv_tabview_set_active(tabview_, static_cast<uint32_t>(next), LV_ANIM_OFF);
    }
    // lv_tabview_set_active() fires LV_EVENT_VALUE_CHANGED, so selectStage()
    // has usually already run by here; calling it again is the no-op guard's
    // job and covers the case where the tabview is gone.
    selectStage(next);
}

void UIInstrumentPage::moveParam(int delta) {
    Param params[kMaxParams];
    const int n = paramsForStage(static_cast<Stage>(stage_), params, kMaxParams);
    if (n == 0) {
        return;
    }
    param_ = (param_ + delta + n) % n;
    refreshParams();
}

void UIInstrumentPage::onInput(const InputEvent& evt) {
    switch (evt.type) {
        // steps() carries the direction, so neither case negates anything.
        // Negating `delta` here used to invert the value: the rotary encoder
        // posted a signed count, so counter-clockwise arrived negative and
        // `-delta` made it an increase.
        case InputType::EncoderRight:
        case InputType::EncoderLeft:
            if (editing_) {
                stepParam(evt.steps());
            } else {
                // Walking the list stays one entry per event however fast the
                // knob is turned; only the value follows the magnitude.
                moveParam(evt.steps() > 0 ? +1 : -1);
            }
            break;
        case InputType::EncoderUp:
        case InputType::EncoderDown:
            // Clockwise (positive steps()) is the next stage. EncoderDown used
            // to be "next" - compensating for a backwards-counting encoder.
            // Direction is set once per encoder in hardware_config.h
            // (WAVEX_*_DIRECTION), never in a page.
            moveStage(evt.steps() > 0 ? +1 : -1);
            break;
        case InputType::EncoderClick:
            // Click toggles between walking the list and changing the value, so
            // one encoder covers both without a modifier.
            editing_ = !editing_;
            refreshStatus(editing_ ? "Editing value - click again to move between parameters" : "");
            refreshParams();
            break;
        default:
            break;
    }
}

std::array<Softkey, NUM_SOFTKEYS> UIInstrumentPage::getSoftkeys() {
    std::array<Softkey, NUM_SOFTKEYS> keys{};
    keys[0] = {"Back", []() { UINavigator::instance().pop(); }};
    // Moving BETWEEN PARAMS has no touch equivalent - unlike stage, which the
    // tab bar already exposes to touch, so it moved to the shifted bank
    // below. Without this, reaching anything past the first param in a stage
    // (e.g. SLOT, the second row on Sample) was encoder-only and easy to
    // miss entirely - which is exactly the gap a bench session hit.
    keys[1] = {"< Param", [this]() { moveParam(-1); }};
    keys[2] = {"Param >", [this]() { moveParam(+1); }};
    keys[3] = {"Value -", [this]() { stepParam(-1); }};
    keys[4] = {"Value +", [this]() { stepParam(+1); }};
    keys[5] = {editing_ ? "Edit*" : "Edit", [this]() {
                   editing_ = !editing_;
                   refreshParams();
                   UINavigator::instance().refreshSoftkeys();
               }};
    return keys;
}

std::array<Softkey, NUM_SOFTKEYS> UIInstrumentPage::getShiftedSoftkeys() {
    std::array<Softkey, NUM_SOFTKEYS> keys{};
    keys[0] = {"Back", []() { UINavigator::instance().pop(); }};
    // The tab bar is also a touch route between stages; these are the same
    // move without reaching for the screen. Moved here (off the primary
    // bank) to make room for < Param/Param > above, which has no touch
    // equivalent at all.
    keys[1] = {"< Stage", [this]() { moveStage(-1); }};
    keys[2] = {"Stage >", [this]() { moveStage(+1); }};
    // Save/Load are shown but unwired, and say why: an Instrument needs the
    // .wxi file and protocol messages that do not exist (track-and-patch-model
    // stage 4). Better a labelled gap than a button that appears to work.
    keys[3] = {"Save", nullptr, false, "needs the Instrument file (.wxi)"};
    keys[4] = {"Load", nullptr, false, "needs the Instrument file (.wxi)"};
    keys[5] = {"Init", [this]() {
                   // Re-send every wired parameter at its default so the engine
                   // and the page agree again.
                   for (int s = 0; s < kStageCount; ++s) {
                       Param params[kMaxParams];
                       const int n = paramsForStage(static_cast<Stage>(s), params, kMaxParams);
                       for (int i = 0; i < n; ++i) {
                           stage_values_[s][i] = params[i].value;
                           if (params[i].wire_param != kParamNone) {
                               sendParam(params[i]);
                           }
                       }
                   }
                   snprintf(instrument_name_, sizeof(instrument_name_), "Init Instrument");
                   refreshHeader();
                   refreshParams();
                   refreshStatus("Instrument reset to defaults");
               }};
    return keys;
}

void UIInstrumentPage::onTrackChanged() {
    // The panel's Track -/+ stepped the shared Track. Unlike the TRACK param
    // this does not re-bind the sample: the global key selects a Track, it
    // does not edit one.
    refreshHeader();
    refreshParams();
}

std::shared_ptr<UIPage> createInstrumentPage() {
    return std::make_shared<UIInstrumentPage>();
}

}  // namespace wavex_ui
