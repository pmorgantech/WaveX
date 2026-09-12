// WaveX Instrument editor
#include "ui/ui_instrument_page.h"

#include <esp_log.h>
#include <esp_random.h>

#include "../styles/ui_theme.h"
#include "debug/console_command.h"
#include "inter_mcu.h"
#include "ui/current_track.h"
#include "ui/ui_navigator.h"
#include "ui/ui_pad_map_page.h"
#include "ui/ui_palette.h"
#include "ui/ui_sample_browser.h"
#include "ui/ui_tab_group.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace wavex_ui {

using namespace wavex_ui::palette;

namespace {
static const char* TAG = "UI_INSTRUMENT";
uint32_t nextId() {
    static uint32_t id = esp_random();
    if (++id == 0)
        ++id;
    return id;
}
constexpr uint8_t kParamFilterMode = 0xFC;  // page-local field, never a control-change id
constexpr const char* filterModes[] = {"Low-pass", "High-pass", "Band-pass", "Notch"};
constexpr const char* envelopeFields[] = {"ATTACK", "DECAY", "SUSTAIN", "RELEASE"};
constexpr const char* slotFields[] = {"SOURCE", "DEST", "DEPTH", "CURVE", "POLARITY"};
constexpr uint8_t liveSources[] = {0, 1, 2, 15, 3, 16, 6, 17, 4, 7};
const char* sourceName(int source) {
    switch (source) {
        case 0:
            return "None";
        case 1:
            return "Vel";
        case 2:
            return "Note";
        case 15:
            return "Env 1";
        case 3:
            return "Env 2";
        case 16:
            return "Env 3";
        case 6:
            return "V LFO 1";
        case 17:
            return "V LFO 2";
        case 4:
            return "G LFO";
        case 7:
            return "Random";
        default:
            return "Pending";
    }
}
bool liveSource(int source) {
    for (auto value: liveSources)
        if (value == source)
            return true;
    return false;
}
constexpr const char* oscillatorFields[] = {"LEVEL", "MIX", "COARSE", "FINE", "KEYTRACK"};

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
const char* const lfoFields[] = {"WAVE", "RATE", "SYNC", "RETRIGGER", "DELAY", "FADE", "FOLLOW"};
const char* const kStageNames[] = {"Osc", "Env", "Amp", "Filter", "Mod", "LFO"};

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

// Other stages use one row of tiles, centred in the body. Osc has
// six controls and Amp two, so the row cannot fill 501px without stretching
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
    auto add = [&](const char* label, uint8_t wire, int32_t value, const char* unit) {
        if (n < max) {
            out[n++] = Param{label, wire, value, s, unit};
        }
    };

    switch (s) {
        case Stage::Oscillator:
            add("OSC", kParamOscillator, 0, "");
            add("LEVEL", kParamOscillator, 0, "%");
            add("MIX", kParamOscillator, 0, "% Osc 2");
            add("COARSE", kParamOscillator, 0, "semitones");
            add("FINE", kParamOscillator, 0, "cents");
            add("KEYTRACK", kParamOscillator, 0, "");
            break;
        case Stage::Envelopes:
            for (uint8_t i = 0; i < 4; ++i)
                add(envelopeFields[i], kParamModulator, modulator_.Value(i), i == 2 ? "%" : "ms");
            break;
        case Stage::Lfo:
            add("LFO", kParamModulator, lfo_.Index() + 1, "");
            for (uint8_t i = 0; i < 7; ++i)
                add(lfoFields[i],
                    kParamModulator,
                    lfo_.Value(i),
                    i == 1             ? "Hz (Sync off)"
                    : i == 4 || i == 5 ? "ms"
                                       : "");
            break;
        case Stage::Amp:
            add("LEVEL", WaveX::Protocol::PARAM_GAIN, sound_.Value(2), "%");
            add("PAN", WaveX::Protocol::PARAM_PAN, sound_.Value(3), "%");
            break;
        case Stage::Filter:
            add("CUTOFF", WaveX::Protocol::PARAM_FILTER_CUTOFF, sound_.Value(0), "%");
            add("RES", WaveX::Protocol::PARAM_FILTER_RESONANCE, sound_.Value(1), "%");
            add("TYPE", kParamFilterMode, sound_.Value(4), "");
            break;
        case Stage::Mod:
            add("SLOT", kParamModulator, selected_slot_ + 1, "");
            for (uint8_t i = 0; i < 5; ++i)
                add(slotFields[i],
                    kParamModulator,
                    modulator_.Value(i),
                    i == 2   ? "%"
                    : i == 4 ? "Center: 0..1 to -1..1"
                             : "");
            break;
        default:
            break;
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

// Shared Track selection is also used by Play, Browser and Track pages.
uint8_t UIInstrumentPage::currentTrack() const {
    return display_track_;
}

void UIInstrumentPage::onEnter(lv_obj_t* parent) {
    lv_obj_clean(parent);
    display_track_ = getCurrentTrack();
    lfo_.Reset(display_track_);
    sound_.Reset(display_track_);
    requested_action_ = 0;
    action_read_id_ = 0;
    track_change_pending_ = false;
    stage_ = 0;
    param_ = 0;
    editing_ = false;
    status_[0] = 0;
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

    // Entering this page reads the Track; only explicit edits mutate it.
    seedValues();

    lfo_.Reset(currentTrack());
    oscillator_.Reset(currentTrack(), 0);
    modulator_.Reset(currentTrack());
    selected_env_ = selected_slot_ = 0;
    mod_pending_at_ = 0;
    mod_timed_out_ = false;
    alive_ = inter_mcu_backend_link_alive();
    timed_out_ = false;
    pending_at_ = 0;
    readOscillator();
    readSound();
    timer_ = lv_timer_create(tick, 50, this);

    // Only the first tab's widgets exist after this; the rest are built when
    // first shown.
    buildStageRows(stage_);
    refreshHeader();
    refreshParams();
}

bool UIInstrumentPage::canLeave() {
    if (!draftActive() || !alive_)
        return true;
    refreshStatus("Updating sound...");
    return false;
}
void UIInstrumentPage::onExit() {
    if (timer_)
        lv_timer_delete(timer_);
    timer_ = nullptr;
    oscillator_status_ = nullptr;
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
            // Dragging a dial focuses it and then takes exactly the encoder's
            // path, so touch and encoder cannot produce different results from
            // the same movement.
            dialSetOnAdjust(dials_[i], [this, i](int steps) {
                param_ = i;
                stepParam(steps);
                refreshParams();
            });
        }
        env_curve_ = buildCurvePane(body,
                                    kEnvCurveX,
                                    kBodyPadTop,
                                    kEnvCurveW,
                                    kPaneH,
                                    "ENVELOPE",
                                    "ADSR",
                                    env_pts_,
                                    kEnvCurvePoints);
    } else if (stage == static_cast<int>(Stage::Filter)) {
        filter_curve_ = buildCurvePane(body,
                                       UI_MARGIN_X,
                                       kBodyPadTop,
                                       kContentW,
                                       kFilterCurveH,
                                       "FILTER RESPONSE",
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
            if (params[i].wire_param != kParamNone) {
                const int idx = i;
                valueTileSetOnAdjust(tiles_[stage][i], [this, idx](int steps) {
                    param_ = idx;
                    stepParam(steps);
                    refreshParams();
                });
            }
        }
    } else {
        // One row of tiles, divided evenly. Osc has six, Amp two, Mod
        // three; an even division keeps every stage on the same baseline
        // rather than giving each its own bespoke grid.
        const bool lfo = stage == static_cast<int>(Stage::Lfo);
        const int cols = lfo ? 4 : n > 0 ? n : 1;
        const int tw = (kContentW - (cols - 1) * kDialGap) / cols;
        const int tile_height = lfo ? (kBodyH - 2 * kBodyPadTop - kDialGap) / 2 : kRowTileH;
        const int ty = lfo ? kBodyPadTop : (kBodyH - kRowTileH) / 2;
        for (int i = 0; i < n; ++i) {
            tiles_[stage][i] = valueTileCreate(body,
                                               UI_MARGIN_X + (i % cols) * (tw + kDialGap),
                                               ty + (i / cols) * (tile_height + kDialGap),
                                               tw,
                                               tile_height,
                                               params[i].label,
                                               (stage == static_cast<int>(Stage::Oscillator) ||
                                                stage == static_cast<int>(Stage::Mod) || lfo)
                                                   ? ""
                                                   : params[i].unit);
            if (stage == static_cast<int>(Stage::Oscillator) ||
                stage == static_cast<int>(Stage::Mod) || lfo) {
                tiles_[stage][i].value_font = UI_FONT_MONO_VALUE;
                if (params[i].unit[0])
                    valueTileSetDesc(tiles_[stage][i], params[i].unit);
            }
            if (params[i].wire_param != kParamNone) {
                const int idx = i;
                valueTileSetOnAdjust(tiles_[stage][i], [this, idx](int steps) {
                    param_ = idx;
                    stepParam(steps);
                    refreshParams();
                });
            }
        }
    }

    if (stage == static_cast<int>(Stage::Oscillator)) {
        oscillator_status_ = lv_label_create(body);
        ui_theme_apply_label_style(oscillator_status_, false);
        lv_obj_set_style_text_font(oscillator_status_, UI_FONT_SMALL, 0);
        lv_obj_set_pos(
            oscillator_status_, UI_MARGIN_X, (kBodyH + kRowTileH) / 2 + UI_PADDING_LARGE);
        lv_obj_set_width(oscillator_status_, kContentW);
        lv_label_set_long_mode(oscillator_status_, LV_LABEL_LONG_WRAP);
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

    const float a = static_cast<float>(params[0].value) / 1000.0f;
    const float d = static_cast<float>(params[1].value) / 1000.0f;
    const float sus = static_cast<float>(params[2].value) / 1000.0f;
    const float r = static_cast<float>(params[3].value) / 1000.0f;

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

// Illustrative mode/cutoff/resonance shape. This is not the engine's actual
// transfer function or a measured response.
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
        const int mode = sound_.Value(4);
        float x = t - cutoff;  // illustrative shape, not a measured response
        if (mode == 1)
            x = -x;
        if (mode == 2)
            x = std::fabs(x);
        if (mode == 3)
            x = .25f - std::fabs(x);
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
    if (draftActive()) {
        lv_tabview_set_active(tabview_, stage_, LV_ANIM_OFF);
        refreshStatus("Updating sound...");
        return;
    }
    stage_ = stage;
    readSound();
    if (lfoStage())
        readLfo();
    if (modStage()) {
        modulator_.Select(
            stage_ == static_cast<int>(Stage::Envelopes),
            stage_ == static_cast<int>(Stage::Envelopes) ? selected_env_ : selected_slot_);
        readModulator();
    }
    status_[0] = 0;
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
    WaveX::Protocol::TrackBindingMessage binding;
    const char* name = "Empty";
    if (inter_mcu_get_track_binding(currentTrack(), &binding)) {
        if (binding.name[0])
            name = binding.name;
        else if (binding.state == WaveX::Protocol::TRACK_BINDING_SAMPLE)
            name = "Sample";
        else if (binding.state == WaveX::Protocol::TRACK_BINDING_LOADING)
            name = "Loading";
    }
    snprintf(context_line_,
             sizeof(context_line_),
             "Track %u / %.23s%s",
             trackDisplayNumber(currentTrack()),
             name,
             sound_.Dirty() ? " / Edited" : "");
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
    if (lfoStage()) {
        refreshLfo();
        return;
    }
    if (modStage()) {
        refreshModulator();
        return;
    }
    if (stage_ == static_cast<int>(Stage::Oscillator)) {
        refreshOscillator();
        return;
    }
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
        const bool amp = stage_ == static_cast<int>(Stage::Amp);
        const bool mode = p.wire_param == kParamFilterMode;
        const float frac = static_cast<float>(p.value) / (mode ? 3.f : amp ? 1000.f : 65535.f);

        ValueTile& tile = tiles_[stage_][i];
        if (!tile.card) {
            continue;
        }
        if (inert) {
            // Set once: valueTileSetUnwired() adds a reason label each call.
            if (!tile.note) {
                valueTileSetUnwired(tile,
                                    stage_ == static_cast<int>(Stage::Mod)
                                        ? "Modulation editor pending"
                                        : "Instrument editor pending");
            }
            continue;
        }

        char value[40];
        if (mode)
            snprintf(value, sizeof(value), "%s", filterModes[std::clamp<int>(p.value, 0, 3)]);
        else
            snprintf(value, sizeof(value), "%d", static_cast<int>(frac * 100.0f + 0.5f));
        valueTileSetValue(tile, value);
        valueTileSetFill(tile, std::clamp(frac, 0.f, 1.f));
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
        refreshStatus("This Instrument control is not implemented yet");
        return;
    }
    if (p.wire_param == kParamOscillator || p.wire_param == kParamModulator)
        return;
    if (requested_action_)
        return;
    const uint8_t field = p.wire_param == kParamFilterMode                          ? 4
                          : p.wire_param == WaveX::Protocol::PARAM_FILTER_CUTOFF    ? 0
                          : p.wire_param == WaveX::Protocol::PARAM_FILTER_RESONANCE ? 1
                          : p.wire_param == WaveX::Protocol::PARAM_GAIN             ? 2
                                                                                    : 3;
    if (!sound_.Set(field, p.value))
        refreshStatus("Instrument is not ready");
}

void UIInstrumentPage::stepParam(int steps) {
    if (requested_action_ || track_change_pending_)
        return;
    Param params[kMaxParams];
    const int n = paramsForStage(static_cast<Stage>(stage_), params, kMaxParams);
    if (param_ < 0 || param_ >= n) {
        return;
    }
    Param& p = params[param_];
    if (p.wire_param == kParamNone) {
        refreshStatus("This Instrument control is not implemented yet");
        return;
    }
    if (stage_ == static_cast<int>(Stage::Oscillator)) {
        if (param_ == 0) {
            selectOscillator(static_cast<uint8_t>(
                std::clamp(static_cast<int>(oscillator_.Snapshot().oscillator) + steps, 0, 1)));
        } else if (alive_) {
            const auto field = static_cast<uint8_t>(param_ - 1);
            const int step = field < 2 ? 10 : 1;
            const int value = static_cast<int>(std::clamp(
                static_cast<int64_t>(oscillator_.Value(field)) + static_cast<int64_t>(steps) * step,
                int64_t{-128},
                int64_t{64000}));
            oscillator_.Set(field, value);
            timed_out_ = false;
            refreshOscillator();
            UINavigator::instance().refreshSoftkeys();
        }
        return;
    }

    if (lfoStage()) {
        if (param_ == 0)
            selectLfo(std::clamp<int64_t>(static_cast<int64_t>(lfo_.Index()) + steps, 0, 1));
        else if (alive_) {
            const uint8_t field = static_cast<uint8_t>(param_ - 1);
            const int old = lfo_.Value(field);
            const int step = field == 1                 ? (old < 1000 ? 10 : 100)
                             : field == 4 || field == 5 ? (old < 100    ? 1
                                                           : old < 1000 ? 10
                                                                        : 100)
                                                        : 1;
            lfo_.Set(field,
                     static_cast<int>(std::clamp<int64_t>(
                         static_cast<int64_t>(old) + static_cast<int64_t>(steps) * step,
                         field == 1 ? 20 : 0,
                         LfoModel::Maximum(field))));
            refreshLfo();
            UINavigator::instance().refreshSoftkeys();
        }
        return;
    }
    if (modStage()) {
        if (!alive_)
            return;
        if (!modulator_.Envelope() && param_ == 0) {
            selectModulator(static_cast<int>(
                std::clamp<int64_t>(static_cast<int64_t>(selected_slot_) + steps, 0, 7)));
            return;
        }
        const uint8_t field = static_cast<uint8_t>(modulator_.Envelope() ? param_ : param_ - 1);
        int value = modulator_.Value(field);
        if (!modulator_.Envelope() && field == 0) {
            int pos = 0;
            for (int i = 0; i < static_cast<int>(sizeof(liveSources)); ++i)
                if (liveSources[i] == value)
                    pos = i;
            value = liveSources[std::clamp<int64_t>(
                static_cast<int64_t>(pos) + steps, 0, sizeof(liveSources) - 1)];
        } else {
            const int step = modulator_.Envelope() ? (field == 2      ? 10
                                                      : value < 100   ? 1
                                                      : value < 1000  ? 10
                                                      : value < 10000 ? 100
                                                                      : 1000)
                             : field == 2          ? 328
                                                   : 1;
            const int low = !modulator_.Envelope() && field == 2 ? -32767 : 0;
            const int high = modulator_.Envelope() ? (field == 2 ? 1000 : 600000)
                             : field == 1          ? WaveX::Protocol::INST_MOD_DEST_COUNT - 1
                             : field == 2          ? 32767
                             : field == 3          ? 2
                                                   : 1;
            value = static_cast<int>(std::clamp<int64_t>(
                static_cast<int64_t>(value) + static_cast<int64_t>(steps) * step, low, high));
        }
        modulator_.Set(field, value);
        mod_timed_out_ = false;
        refreshModulator();
        UINavigator::instance().refreshSoftkeys();
        return;
    }

    const bool amp = stage_ == static_cast<int>(Stage::Amp);
    const bool mode = p.wire_param == kParamFilterMode;
    const int high = mode ? 3 : amp ? (param_ == 0 ? 64000 : 1000) : 65535;
    p.value = static_cast<int32_t>(std::clamp<int64_t>(
        static_cast<int64_t>(p.value) + static_cast<int64_t>(steps) * (mode  ? 1
                                                                       : amp ? 10
                                                                             : kParamStep),
        0,
        high));
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
    if (stage_ == static_cast<int>(Stage::Oscillator))
        keys[5] = {"Pad Map",
                   [] { UINavigator::instance().push(createPadMapPage()); },
                   !oscillator_.Dirty() && !oscillator_.Pending(),
                   "Updating sound..."};
    if (modStage()) {
        const bool env = modulator_.Envelope();
        const bool navigating = !modulator_.Dirty() && !modulator_.Pending();
        keys[1] = {env ? "< Env" : "< Slot",
                   [this] { selectModulator(modulator_.Index() - 1); },
                   navigating && modulator_.Index() > 0,
                   "Updating sound..."};
        keys[2] = {env ? "Env >" : "Slot >",
                   [this] { selectModulator(modulator_.Index() + 1); },
                   navigating && modulator_.Index() < (env ? 2 : 7),
                   "Updating sound..."};
        keys[3] = {"Apply",
                   [this] { soundAction(WaveX::Protocol::INST_EDIT_APPLY); },
                   alive_ && sound_.Dirty() && !draftActive(),
                   "No edits"};
        keys[4] = {"Revert",
                   [this] { soundAction(WaveX::Protocol::INST_EDIT_REVERT); },
                   alive_ && sound_.Dirty() && !draftActive(),
                   "No edits"};
        keys[5] = env ? Softkey{"Mod",
                                [this] { moveStage(static_cast<int>(Stage::Mod) - stage_); },
                                navigating,
                                "Updating sound..."}
                      : Softkey{"Clear",
                                [this] {
                                    modulator_.Clear();
                                    refreshModulator();
                                    UINavigator::instance().refreshSoftkeys();
                                },
                                alive_ && modulator_.Editable(),
                                "Instrument is busy"};
    }
    if (lfoStage()) {
        const bool navigating = !draftActive();
        keys[1] = {"< LFO",
                   [this] { selectLfo(lfo_.Index() - 1); },
                   navigating && lfo_.Index() > 0,
                   "Updating sound..."};
        keys[2] = {"LFO >",
                   [this] { selectLfo(lfo_.Index() + 1); },
                   navigating && lfo_.Index() < 1,
                   "Updating sound..."};
        keys[3] = {"Apply",
                   [this] { soundAction(WaveX::Protocol::INST_EDIT_APPLY); },
                   alive_ && sound_.Dirty() && navigating,
                   "No edits"};
        keys[4] = {"Revert",
                   [this] { soundAction(WaveX::Protocol::INST_EDIT_REVERT); },
                   alive_ && sound_.Dirty() && navigating,
                   "No edits"};
        keys[5] = {"Mod",
                   [this] { moveStage(static_cast<int>(Stage::Mod) - stage_); },
                   navigating,
                   "Updating sound..."};
    }
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
    keys[3] = {
        "Key Map", [this] {
            UINavigator::instance().push(createKeyMapPage(oscillator_.Snapshot().oscillator));
        }};
    keys[4] = {"Browse", [] {
                   if (auto page = createInstrumentBrowserPage())
                       UINavigator::instance().push(page);
               }};
    keys[3].enabled = keys[4].enabled = !draftActive();
    keys[3].why = keys[4].why = "Updating sound...";
    if (stage_ == static_cast<int>(Stage::Oscillator)) {
        const bool ready = alive_ && oscillator_.Editable();
        const bool navigating = !oscillator_.Dirty() && !oscillator_.Pending();
        keys[3].enabled = keys[4].enabled = navigating;
        keys[3].why = keys[4].why = "Updating sound...";
        keys[5] = {"Copy Other",
                   [this] { applyOscillator(WaveX::Protocol::INST_OSC_COPY_EMPTY); },
                   ready && navigating && oscillator_.Snapshot().zones == 0,
                   "Select an empty oscillator"};
    }
    keys[1] = {"Apply",
               [this] { soundAction(WaveX::Protocol::INST_EDIT_APPLY); },
               alive_ && sound_.Dirty() && !draftActive(),
               "No edits"};
    keys[2] = {"Revert",
               [this] { soundAction(WaveX::Protocol::INST_EDIT_REVERT); },
               alive_ && sound_.Dirty() && !draftActive(),
               "No edits"};
    return keys;
}

void UIInstrumentPage::onTrackChanged() {
    if (draftActive() && alive_) {
        track_change_pending_ = true;
        return;
    }
    track_change_pending_ = false;
    display_track_ = getCurrentTrack();
    lfo_.Reset(display_track_);
    if (lfoStage())
        readLfo();
    sound_.Reset(display_track_);
    requested_action_ = 0;
    action_read_id_ = 0;
    readSound();
    modulator_.Reset(currentTrack());
    modulator_.Select(stage_ != static_cast<int>(Stage::Mod),
                      stage_ == static_cast<int>(Stage::Mod) ? selected_slot_ : selected_env_);
    mod_pending_at_ = 0;
    mod_timed_out_ = false;
    if (modStage())
        readModulator();
    oscillator_.Reset(currentTrack(), oscillator_.Snapshot().oscillator);
    timed_out_ = false;
    pending_at_ = 0;
    readOscillator();
    refreshHeader();
    refreshParams();
}

void UIInstrumentPage::tick(lv_timer_t* timer) {
    auto* page = static_cast<UIInstrumentPage*>(lv_timer_get_user_data(timer));
    page->serviceOscillator();
    page->serviceModulator();
    page->serviceLfo();
    page->serviceSound();
    if (page->track_change_pending_ && !page->draftActive())
        page->onTrackChanged();
}
void UIInstrumentPage::readOscillator() {
    if (!alive_)
        return;
    auto request = oscillator_.Request(nextId(), WaveX::Protocol::INST_OSC_GET);
    if (inter_mcu_send_oscillator(request) == ESP_OK)
        oscillator_.Expect(request.request_id);
    read_at_ = lv_tick_get();
}
void UIInstrumentPage::serviceOscillator() {
    bool changed = false;
    const bool alive = inter_mcu_backend_link_alive();
    if (alive != alive_) {
        alive_ = alive;
        lfo_.Reset(currentTrack());
        if (alive_ && lfoStage())
            readLfo();
        oscillator_.Reset(currentTrack(), oscillator_.Snapshot().oscillator);
        sound_.Reset(currentTrack());
        requested_action_ = 0;
        action_read_id_ = 0;
        modulator_.Reset(currentTrack());
        modulator_.Select(stage_ != static_cast<int>(Stage::Mod),
                          stage_ == static_cast<int>(Stage::Mod) ? selected_slot_ : selected_env_);
        mod_pending_at_ = 0;
        pending_at_ = 0;
        changed = true;
        if (alive_) {
            readOscillator();
            readSound();
        }
    }
    const bool was_pending = oscillator_.Pending();
    WaveX::Protocol::InstOscSyncMessage received;
    if (alive_ && inter_mcu_get_oscillator(&received) && oscillator_.Accept(received)) {
        changed = true;
        if (was_pending && !oscillator_.Pending() && !received.error) {
            lfo_.AdoptRevision(received.revision);
            sound_.AdoptRevision(received.revision);
            modulator_.AdoptRevision(received.revision);
        }
        if (!oscillator_.Pending()) {
            pending_at_ = 0;
            readSound();
        }
    }
    const uint32_t now = lv_tick_get();
    if (oscillator_.Pending() && static_cast<uint32_t>(now - pending_at_) >= 5000) {
        // The reply may be lost after a successful edit. Refresh, never retry
        // a mutation blindly or claim it failed.
        timed_out_ = true;
        oscillator_.Reset(currentTrack(), oscillator_.Snapshot().oscillator);
        pending_at_ = 0;
        changed = true;
        readOscillator();
    }
    if (alive_ && static_cast<uint32_t>(now - read_at_) >= 300 &&
        (stage_ == static_cast<int>(Stage::Oscillator) || oscillator_.Pending()))
        readOscillator();
    if (alive_ && oscillator_.Ready() && oscillator_.Dirty() && !modulator_.Pending() &&
        !sound_.Pending() && !requested_action_ && !lfo_.Pending())
        applyOscillator(WaveX::Protocol::INST_OSC_SET);
    if (changed) {
        refreshHeader();
        refreshParams();
        UINavigator::instance().refreshSoftkeys();
    }
}
void UIInstrumentPage::selectOscillator(uint8_t oscillator) {
    if (oscillator >= 2 || oscillator_.Dirty() || oscillator_.Pending())
        return;
    oscillator_.Reset(currentTrack(), oscillator);
    timed_out_ = false;
    readOscillator();
    refreshParams();
    UINavigator::instance().refreshSoftkeys();
}
void UIInstrumentPage::applyOscillator(uint8_t operation) {
    if (!alive_ || !oscillator_.Ready() || !oscillator_.Editable() ||
        (operation == WaveX::Protocol::INST_OSC_SET && !oscillator_.Dirty()) ||
        (operation == WaveX::Protocol::INST_OSC_COPY_EMPTY &&
         (oscillator_.Dirty() || oscillator_.Snapshot().zones)))
        return;
    auto request = oscillator_.Request(nextId(), operation);
    if (inter_mcu_send_oscillator(request) != ESP_OK) {
        refreshStatus("Send failed");
        return;
    }
    oscillator_.MutationSent(request.request_id);
    pending_at_ = read_at_ = lv_tick_get();
    timed_out_ = false;
    refreshOscillator();
    UINavigator::instance().refreshSoftkeys();
}
void UIInstrumentPage::refreshOscillator() {
    const auto& state = oscillator_.Snapshot();
    for (uint8_t i = 0; i < 6; ++i) {
        auto& tile = tiles_[0][i];
        if (!tile.card)
            continue;
        char value[24];
        float fill = 0;
        if (i == 0) {
            snprintf(value, sizeof(value), "%u", state.oscillator + 1);
            fill = state.oscillator;
        } else if (!oscillator_.Valid())
            snprintf(value, sizeof(value), "--");
        else {
            const int v = oscillator_.Value(i - 1);
            if (i <= 2) {
                snprintf(value, sizeof(value), "%d.%d", v / 10, v % 10);
                fill = std::min(static_cast<float>(v) / 1000.0f, 1.0f);
            } else if (i == 5) {
                snprintf(value, sizeof(value), "%s", v ? "On" : "Off");
                fill = static_cast<float>(v);
            } else {
                snprintf(value, sizeof(value), "%+d", v);
                fill = static_cast<float>(v + 128) / 255.0f;
            }
        }
        valueTileSetValue(tile, value);
        valueTileSetFill(tile, fill);
        valueTileSetFocus(tile, i == param_);
    }
    const char* message = !alive_                  ? "Audio engine disconnected"
                          : oscillator_.Pending()  ? "Updating sound..."
                          : timed_out_             ? "Reply timed out; showing refreshed settings"
                          : !oscillator_.Valid()   ? "Reading oscillator..."
                          : !state.valid           ? "Load an Instrument from Browse"
                          : state.busy             ? "Instrument is busy"
                          : oscillator_.Conflict() ? "Instrument changed; draft discarded"
                          : oscillator_.Dirty()    ? "Updating sound..."
                          : state.error            ? "Edit rejected; showing current settings"
                          : state.type == 2        ? "Wavetable playback is not available yet"
                          : state.zones == 0
                              ? "Empty map: Shift > Copy Other to copy the other oscillator"
                              : "Sound controls update held notes. Save a WXI copy from Pad Map.";
    char status[256];
    snprintf(status,
             sizeof(status),
             "Osc %u / %s / %u zone%s\n\n%s",
             state.oscillator + 1,
             state.type == 1   ? "Sample"
             : state.type == 2 ? "Wavetable"
                               : "Off",
             state.zones,
             state.zones == 1 ? "" : "s",
             message);
    if (oscillator_status_ && strcmp(lv_label_get_text(oscillator_status_), status))
        lv_label_set_text(oscillator_status_, status);
}

void UIInstrumentPage::readSound() {
    if (!alive_)
        return;
    const auto request = sound_.Request(nextId(), WaveX::Protocol::INST_EDIT_GET);
    if (inter_mcu_send_instrument_edit(request) == ESP_OK) {
        sound_.Expect(request.request_id);
        if (requested_action_ && !sound_.Pending())
            action_read_id_ = request.request_id;
    }
    sound_read_at_ = lv_tick_get();
}
void UIInstrumentPage::sendSound(uint8_t op) {
    if (!alive_ || !sound_.Ready())
        return;
    const auto request = sound_.Request(nextId(), op);
    if (inter_mcu_send_instrument_edit(request) != ESP_OK) {
        refreshStatus("Send failed");
        return;
    }
    sound_.Sent(request.request_id, op);
    sound_pending_at_ = sound_read_at_ = lv_tick_get();
    if (op == WaveX::Protocol::INST_EDIT_APPLY || op == WaveX::Protocol::INST_EDIT_REVERT)
        action_read_id_ = 0;
}
void UIInstrumentPage::soundAction(uint8_t op) {
    if (!alive_ || draftActive() || !sound_.Dirty())
        return;
    requested_action_ = op;
    sound_pending_at_ = lv_tick_get();
    readSound();  // A fresh revision includes the last preview in any tab.
    UINavigator::instance().refreshSoftkeys();
}
void UIInstrumentPage::serviceSound() {
    bool changed = false;
    const bool was_pending = sound_.Pending();
    WaveX::Protocol::InstEditSyncMessage received;
    if (alive_ && inter_mcu_get_instrument_edit(&received) && sound_.Accept(received)) {
        changed = true;
        if (was_pending && !sound_.Pending() && !received.error && !requested_action_) {
            lfo_.AdoptRevision(received.revision);
            oscillator_.AdoptRevision(received.revision);
            modulator_.AdoptRevision(received.revision);
        }
        if (requested_action_ && !sound_.Pending()) {
            if (action_read_id_ && received.request_id == action_read_id_) {
                sendSound(requested_action_);
            } else if (!action_read_id_) {
                requested_action_ = 0;
                const auto lfo_index = lfo_.Index();
                lfo_.Reset(currentTrack());
                lfo_.Select(lfo_index);
                if (lfoStage())
                    readLfo();
                status_[0] = 0;
                oscillator_.Reset(currentTrack(), oscillator_.Snapshot().oscillator);
                const bool envelope = modulator_.Envelope();
                const uint8_t index = modulator_.Index();
                modulator_.Reset(currentTrack());
                modulator_.Select(envelope, index);
                readOscillator();
                readModulator();
            }
        }
    }
    const uint32_t now = lv_tick_get();
    if ((sound_.Pending() || requested_action_) &&
        static_cast<uint32_t>(now - sound_pending_at_) >= 5000) {
        sound_.Reset(currentTrack());
        requested_action_ = 0;
        action_read_id_ = 0;
        refreshStatus("Readback timed out; checking sound");
        readSound();
        changed = true;
    }
    if (sound_.Outgoing() && sound_.Ready() && !oscillator_.Pending() && !modulator_.Pending() &&
        !requested_action_ && !lfo_.Pending())
        sendSound(sound_.Operation());
    if (alive_ && static_cast<uint32_t>(now - sound_read_at_) >= 300)
        readSound();
    if (changed) {
        refreshHeader();
        refreshParams();
        UINavigator::instance().refreshSoftkeys();
    }
}

void UIInstrumentPage::readLfo() {
    if (!alive_)
        return;
    const auto r = lfo_.Request(nextId(), true);
    if (inter_mcu_send_instrument_lfo(r) == ESP_OK)
        lfo_.Expect(r.request_id);
    lfo_read_at_ = lv_tick_get();
}
void UIInstrumentPage::serviceLfo() {
    bool changed = false;
    const bool pending = lfo_.Pending();
    WaveX::Protocol::InstLfoSyncMessage received;
    if (alive_ && inter_mcu_get_instrument_lfo(&received) && lfo_.Accept(received)) {
        changed = true;
        if (pending && !lfo_.Pending()) {
            if (!received.error) {
                sound_.AdoptRevision(received.revision);
                oscillator_.AdoptRevision(received.revision);
                modulator_.AdoptRevision(received.revision);
            }
            readSound();
        }
    }
    const uint32_t now = lv_tick_get();
    if (lfo_.Pending() && static_cast<uint32_t>(now - lfo_pending_at_) >= 5000) {
        const auto index = lfo_.Index();
        lfo_.Reset(currentTrack());
        lfo_.Select(index);
        refreshStatus("Reply timed out; checking LFO");
        readLfo();
        changed = true;
    }
    if (alive_ && lfo_.Ready() && lfo_.Dirty() && !oscillator_.Pending() && !modulator_.Pending() &&
        !sound_.Pending() && !requested_action_) {
        const auto r = lfo_.Request(nextId());
        if (!WaveX::Protocol::IsValidInstLfoOp(r)) {
            refreshStatus("Select a supported wave and sync");
        } else if (inter_mcu_send_instrument_lfo(r) == ESP_OK) {
            lfo_.Sent(r.request_id);
            lfo_pending_at_ = lfo_read_at_ = now;
            changed = true;
        }
    }
    if (alive_ && (lfoStage() || lfo_.Pending()) &&
        static_cast<uint32_t>(now - lfo_read_at_) >= 300)
        readLfo();
    if (changed) {
        if (lfoStage())
            refreshLfo();
        UINavigator::instance().refreshSoftkeys();
    }
}
void UIInstrumentPage::selectLfo(int index) {
    if (index < 0 || index > 1 || !lfo_.Select(static_cast<uint8_t>(index)))
        return;
    refreshLfo();
    UINavigator::instance().refreshSoftkeys();
}
void UIInstrumentPage::refreshLfo() {
    const char* waves[] = {"Sine", "Triangle", "Saw", "Square", "S & H"};
    const char* sync[] = {"Off", "1/16", "1/8", "1/4", "1/2", "1 bar", "2 bars", "4 bars"};
    for (int i = 0; i < 8; ++i) {
        auto& tile = tiles_[static_cast<int>(Stage::Lfo)][i];
        if (!tile.card)
            continue;
        char value[24];
        float fill = 0;
        if (i == 0) {
            snprintf(value, sizeof(value), "%u", lfo_.Index() + 1);
            fill = lfo_.Index();
        } else if (!lfo_.Valid())
            snprintf(value, sizeof(value), "--");
        else {
            const int v = lfo_.Value(static_cast<uint8_t>(i - 1));
            fill = std::clamp(static_cast<float>(v) / LfoModel::Maximum(i - 1), 0.f, 1.f);
            if (i == 1)
                snprintf(value, sizeof(value), "%s", v <= 4 ? waves[v] : "Unknown");
            else if (i == 2)
                snprintf(value, sizeof(value), "%.2f", static_cast<double>(v) / 1000);
            else if (i == 3)
                snprintf(value, sizeof(value), "%s", v <= 7 ? sync[v] : "Unknown");
            else if (i == 4)
                snprintf(value, sizeof(value), "%s", v ? "Gate" : "Free");
            else if (i == 7)
                snprintf(value, sizeof(value), "%s", v ? "On (Hz)" : "Off");
            else
                snprintf(value, sizeof(value), "%d", v);
        }
        valueTileSetValue(tile, value);
        valueTileSetFill(tile, fill);
        valueTileSetFocus(tile, i == param_);
    }
    const auto& state = lfo_.Snapshot();
    refreshStatus(!alive_                          ? "Audio engine disconnected"
                  : !lfo_.Valid()                  ? "Reading LFO..."
                  : !state.valid                   ? "Load an Instrument"
                  : state.busy                     ? "Instrument is busy"
                  : lfo_.Dirty() || lfo_.Pending() ? "Updating sound..."
                  : state.error                    ? "Edit rejected; current settings shown"
                                                   : "Route through Mod");
}
void UIInstrumentPage::readModulator() {
    if (!alive_)
        return;
    const auto request = modulator_.Request(nextId(), true);
    if (inter_mcu_send_modulator(request) == ESP_OK)
        modulator_.Expect(request.request_id);
    mod_read_at_ = lv_tick_get();
}
void UIInstrumentPage::serviceModulator() {
    bool changed = false;
    const bool was_pending = modulator_.Pending();
    WaveX::Protocol::InstModSyncMessage received;
    if (alive_ && inter_mcu_get_modulator(&received) && modulator_.Accept(received)) {
        changed = true;
        if (was_pending && !modulator_.Pending() && !received.error) {
            lfo_.AdoptRevision(received.revision);
            sound_.AdoptRevision(received.revision);
            oscillator_.AdoptRevision(received.revision);
        }
        if (!modulator_.Pending()) {
            mod_pending_at_ = 0;
            readSound();
        }
    }
    const uint32_t now = lv_tick_get();
    if (modulator_.Pending() && static_cast<uint32_t>(now - mod_pending_at_) >= 5000) {
        const bool envelope = modulator_.Envelope();
        const uint8_t index = modulator_.Index();
        modulator_.Reset(currentTrack());
        modulator_.Select(envelope, index);
        mod_pending_at_ = 0;
        mod_timed_out_ = true;
        changed = true;
        readModulator();
    }
    if (alive_ && static_cast<uint32_t>(now - mod_read_at_) >= 300 &&
        (modStage() || modulator_.Pending()))
        readModulator();
    if (alive_ && modulator_.Ready() && modulator_.Dirty() && !oscillator_.Pending() &&
        !sound_.Pending() && !requested_action_ && !lfo_.Pending())
        applyModulator();
    if (changed && modStage()) {
        refreshModulator();
        UINavigator::instance().refreshSoftkeys();
    }
}
void UIInstrumentPage::selectModulator(int index) {
    if (index < 0 || index >= (modulator_.Envelope() ? 3 : 8))
        return;
    if (!modulator_.Select(modulator_.Envelope(), static_cast<uint8_t>(index)))
        return;
    if (modulator_.Envelope())
        selected_env_ = static_cast<uint8_t>(index);
    else
        selected_slot_ = static_cast<uint8_t>(index);
    mod_timed_out_ = false;
    refreshModulator();
    UINavigator::instance().refreshSoftkeys();
}
void UIInstrumentPage::applyModulator() {
    if (!alive_ || !modulator_.Ready() || !modulator_.Editable() || !modulator_.Dirty())
        return;
    const auto request = modulator_.Request(nextId());
    if (!WaveX::Protocol::IsValidInstModOp(request)) {
        refreshStatus("Unsupported route; clear it before editing");
        return;
    }
    if (inter_mcu_send_modulator(request) != ESP_OK) {
        refreshStatus("Send failed");
        return;
    }
    modulator_.MutationSent(request.request_id);
    mod_pending_at_ = mod_read_at_ = lv_tick_get();
    mod_timed_out_ = false;
    refreshModulator();
    UINavigator::instance().refreshSoftkeys();
}
void UIInstrumentPage::refreshModulator() {
    const bool env = modulator_.Envelope();
    for (int i = 0; i < (env ? 4 : 6); ++i) {
        char value[24];
        float fill = 0;
        const int v = modulator_.Value(static_cast<uint8_t>(env ? i : i == 0 ? 0 : i - 1));
        if (!env && i == 0) {
            snprintf(value, sizeof(value), "%u", modulator_.Index() + 1);
            fill = static_cast<float>(modulator_.Index()) / 7;
        } else if (!modulator_.Valid())
            snprintf(value, sizeof(value), "--");
        else if (env) {
            if (i == 2)
                snprintf(value, sizeof(value), "%d.%d", v / 10, v % 10);
            else
                snprintf(value, sizeof(value), "%d", v);
            fill = static_cast<float>(v) / (i == 2 ? 1000 : 600000);
        } else {
            const char* destinations[] = {"None", "Cutoff", "Gain", "Pitch", "Pan", "Resonance"};
            const char* curves[] = {"Linear", "Exp", "S"};
            if (i == 1) {
                snprintf(value, sizeof(value), "%s", sourceName(v));
                for (size_t j = 0; j < sizeof(liveSources); ++j)
                    if (liveSources[j] == v)
                        fill = static_cast<float>(j) / (sizeof(liveSources) - 1);
            } else if (i == 2) {
                snprintf(value,
                         sizeof(value),
                         "%s",
                         v < WaveX::Protocol::INST_MOD_DEST_COUNT ? destinations[v] : "Pending");
                fill = v < WaveX::Protocol::INST_MOD_DEST_COUNT
                           ? static_cast<float>(v) / (WaveX::Protocol::INST_MOD_DEST_COUNT - 1)
                           : 0;
            } else if (i == 3) {
                snprintf(value, sizeof(value), "%+.1f", static_cast<double>(v) * 100 / 32767);
                fill = static_cast<float>(v + 32767) / 65534;
            } else if (i == 4) {
                snprintf(value, sizeof(value), "%s", v < 3 ? curves[v] : "Pending");
                fill = v < 3 ? static_cast<float>(v) / 2 : 0;
            } else {
                snprintf(value,
                         sizeof(value),
                         "%s",
                         v == 0   ? "Native"
                         : v == 1 ? "Center"
                                  : "Unknown");
                fill = v == 1 ? 1 : 0;
            }
        }
        if (env) {
            if (!dials_[i].card)
                continue;
            dialSetValue(dials_[i], fill, value, i == 2 ? "%" : "ms");
            dialSetFocus(dials_[i], i == param_);
        } else {
            auto& tile = tiles_[static_cast<int>(Stage::Mod)][i];
            if (!tile.card)
                continue;
            valueTileSetValue(tile, value);
            valueTileSetFill(tile, fill);
            valueTileSetFocus(tile, i == param_);
        }
    }
    if (env)
        refreshEnvCurve();
    const auto& state = modulator_.Snapshot();
    const char* message = !alive_                          ? "Audio engine disconnected"
                          : modulator_.Pending()           ? "Updating sound..."
                          : mod_timed_out_                 ? "Reply timed out; settings refreshed"
                          : !modulator_.Valid()            ? "Reading..."
                          : !state.valid                   ? "Load an Instrument"
                          : state.busy                     ? "Instrument is busy"
                          : modulator_.Conflict()          ? "Instrument changed; draft discarded"
                          : modulator_.Dirty()             ? "Updating sound..."
                          : state.error                    ? "Edit rejected; current settings shown"
                          : env && modulator_.Index() == 0 ? "Amp envelope"
                          : env                            ? "Route through Mod"
                                                           : "Save a WXI copy from Pad Map";
    char status[64];
    snprintf(status,
             sizeof(status),
             "%s %u / %s",
             env ? "Env" : "Slot",
             modulator_.Index() + 1,
             message);
    if (strcmp(status_, status))
        refreshStatus(status);
}
size_t UIInstrumentPage::consoleState(char* out, size_t cap, size_t len) {
    using namespace WaveX::Debug;
    len = AppendKvText(out, cap, len, "tab", kStageNames[stage_]);
    len = AppendKvInt(out, cap, len, "editready", alive_ && sound_.Ready());
    len = AppendKvInt(out, cap, len, "editdirty", sound_.Dirty());
    len = AppendKvInt(out, cap, len, "editpending", draftActive());
    len = AppendKvInt(out, cap, len, "editerror", sound_.Snapshot().error);
    len = AppendKvInt(out, cap, len, "instgain", sound_.Value(2));
    len = AppendKvInt(out, cap, len, "instpan", sound_.Value(3));
    len = AppendKvInt(out, cap, len, "instcutoff", sound_.Value(0));
    len = AppendKvInt(out, cap, len, "instres", sound_.Value(1));
    len = AppendKvInt(out, cap, len, "filtermode", sound_.Value(4));
    if (lfoStage()) {
        len = AppendKvInt(out, cap, len, "lfoready", alive_ && lfo_.Ready());
        len = AppendKvInt(out, cap, len, "lfovalid", lfo_.Snapshot().valid);
        len = AppendKvInt(out, cap, len, "lfodirty", lfo_.Dirty());
        len = AppendKvInt(out, cap, len, "lfopending", lfo_.Pending());
        len = AppendKvInt(out, cap, len, "lfoerror", lfo_.Snapshot().error);
        len = AppendKvInt(out, cap, len, "lfo", lfo_.Index() + 1);
        const char* fields[] = {"wave", "rate", "sync", "retrigger", "delay", "fade", "follow"};
        for (uint8_t i = 0; i < 7; ++i)
            len = AppendKvInt(out, cap, len, fields[i], lfo_.Value(i));
        return len;
    }
    if (modStage()) {
        len = AppendKvInt(out, cap, len, "modready", alive_ && modulator_.Ready());
        len = AppendKvInt(
            out, cap, len, "modvalid", modulator_.Valid() && modulator_.Snapshot().valid);
        len = AppendKvInt(out, cap, len, "moddirty", modulator_.Dirty());
        len = AppendKvInt(out, cap, len, "modpending", modulator_.Pending());
        len = AppendKvInt(out, cap, len, "moderror", modulator_.Snapshot().error);
        len = AppendKvInt(
            out, cap, len, modulator_.Envelope() ? "env" : "slot", modulator_.Index() + 1);
        const char* env[] = {"attack", "decay", "sustain", "release"};
        const char* slot[] = {"source", "dest", "moddepth", "curve", "polarity"};
        for (uint8_t i = 0; i < (modulator_.Envelope() ? 4 : 5); ++i)
            len = AppendKvInt(
                out, cap, len, modulator_.Envelope() ? env[i] : slot[i], modulator_.Value(i));
        return len;
    }
    len = AppendKvInt(out, cap, len, "oscready", alive_ && oscillator_.Ready());
    len =
        AppendKvInt(out, cap, len, "oscvalid", oscillator_.Valid() && oscillator_.Snapshot().valid);
    len = AppendKvInt(out, cap, len, "osc", oscillator_.Snapshot().oscillator + 1);
    len = AppendKvInt(out, cap, len, "osczones", oscillator_.Snapshot().zones);
    len = AppendKvInt(out, cap, len, "oscdirty", oscillator_.Dirty());
    len = AppendKvInt(out, cap, len, "oscerror", oscillator_.Snapshot().error);
    const char* fields[] = {"osclevel", "oscmix", "osccoarse", "oscfine", "osckeytrack"};
    for (uint8_t i = 0; i < 5; ++i)
        len = AppendKvInt(out, cap, len, fields[i], oscillator_.Value(i));
    return len;
}
bool UIInstrumentPage::consoleCommand(const char* args, char* reply, size_t cap) {
    if (requested_action_ || track_change_pending_)
        return false;
    char name[16], extra;
    char tab[16];
    int value;
    if (args && sscanf(args, "%15s %15s %c", name, tab, &extra) == 2 && !strcmp(name, "TAB")) {
        for (int i = 0; i < kStageCount; ++i) {
            if (strcmp(tab, kStageNames[i]))
                continue;
            moveStage(i - stage_);
            snprintf(reply, cap, "ok tab=%s", kStageNames[stage_]);
            return true;
        }
        return false;
    }
    if (args &&
        (stage_ == static_cast<int>(Stage::Amp) || stage_ == static_cast<int>(Stage::Filter)) &&
        sscanf(args, "%15s %d %c", name, &value, &extra) == 2) {
        const bool amp = stage_ == static_cast<int>(Stage::Amp);
        const int field = !amp && !strcmp(name, "TYPE")             ? 4
                          : !strcmp(name, amp ? "LEVEL" : "CUTOFF") ? (amp ? 2 : 0)
                          : !strcmp(name, amp ? "PAN" : "RES")      ? (amp ? 3 : 1)
                                                                    : -1;
        if (field < 0 || !sound_.Set(static_cast<uint8_t>(field), value))
            return false;
        refreshParams();
        snprintf(reply, cap, "ok");
        return true;
    }
    if (args && lfoStage() && sscanf(args, "%15s %d %c", name, &value, &extra) == 2) {
        if (!strcmp(name, "LFO")) {
            if (value < 1 || value > 2 || lfo_.Dirty() || lfo_.Pending())
                return false;
            selectLfo(value - 1);
        } else {
            bool matched = false;
            for (uint8_t i = 0; i < 7; ++i)
                if (!strcmp(name, lfoFields[i]) && alive_) {
                    if (!lfo_.Set(i, value))
                        return false;
                    matched = true;
                }
            if (!matched)
                return false;
            refreshLfo();
            UINavigator::instance().refreshSoftkeys();
        }
        snprintf(reply, cap, "ok");
        return true;
    }
    if (args && modStage() && sscanf(args, "%15s %d %c", name, &value, &extra) == 2) {
        const bool env = modulator_.Envelope();
        if (!strcmp(name, env ? "ENV" : "SLOT")) {
            if (value < 1 || value > (env ? 3 : 8) || modulator_.Dirty() || modulator_.Pending())
                return false;
            selectModulator(value - 1);
        } else {
            bool matched = false;
            for (uint8_t i = 0; i < (env ? 4 : 5); ++i)
                if (!strcmp(name, env ? envelopeFields[i] : slotFields[i]) && alive_) {
                    if ((!env && i == 0 && !liveSource(value)) || !modulator_.Set(i, value))
                        return false;
                    matched = true;
                }
            if (!matched)
                return false;
            refreshModulator();
            UINavigator::instance().refreshSoftkeys();
        }
        snprintf(reply, cap, "ok");
        return true;
    }
    if (!args || stage_ != static_cast<int>(Stage::Oscillator) ||
        sscanf(args, "%15s %d %c", name, &value, &extra) != 2)
        return false;
    if (!strcmp(name, "OSC") && value >= 1 && value <= 2 && !oscillator_.Dirty() &&
        !oscillator_.Pending()) {
        selectOscillator(static_cast<uint8_t>(value - 1));
    } else {
        bool matched = false;
        for (uint8_t i = 0; i < 5; ++i)
            if (!strcmp(name, oscillatorFields[i]) && alive_) {
                const int low = i == 2 || i == 3 ? -128 : 0;
                const int high = i == 0 ? 64000 : i == 1 ? 1000 : i == 4 ? 1 : 127;
                if (value < low || value > high || !oscillator_.Set(i, value))
                    return false;
                matched = true;
            }
        if (!matched)
            return false;
        refreshOscillator();
        UINavigator::instance().refreshSoftkeys();
    }
    snprintf(reply, cap, "ok");
    return true;
}

std::shared_ptr<UIPage> createInstrumentPage() {
    return std::make_shared<UIInstrumentPage>();
}

}  // namespace wavex_ui
