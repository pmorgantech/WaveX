// WaveX Instrument editor (the "Instrument" page; class name follows in the
// mechanical rename)
#include "ui/ui_voice_page.h"

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
static const char* TAG = "UI_VOICE";

// Not in the shared palette: "this control cannot be driven yet" is a state
// only this page and the softkey bar have, and it is not part of the card /
// tab design vocabulary the palette describes.
constexpr uint32_t kColInert = 0x5A5A5A;

// 64 detents end to end: fine enough to sound continuous, coarse enough to
// cross the range without grinding. Matches the Play page's feel.
constexpr int kParamStep = 65535 / 64;

// Tab bar labels, in the same order as UIVoicePage::Stage. Kept short because
// the bar divides evenly - one long label shrinks every other tab's target.
const char* const kStageNames[] = {"Sample", "Env", "Amp", "Filter", "Mod"};

// Content geometry. The panel is 720x1280 rotated to 1280x720; the navigator
// takes UI_HEADER_HEIGHT (75) off the top and UI_HOTKEY_HEIGHT (100) off the
// bottom, the Instrument strip takes 84 more and the tab bar 56. Positions are
// absolute against that, matching the Play and diagnostics pages rather than
// introducing a second convention.
constexpr int kDesignW = 1280;
constexpr int kContentH = 720 - UI_HEADER_HEIGHT - UI_HOTKEY_HEIGHT;  // 545
constexpr int kStripH = 84;
constexpr int kTabBodyH = kContentH - kStripH - 56;  // 405, after the tab bar

constexpr int kPanelX = 12;
constexpr int kPanelY = 8;
constexpr int kPanelW = kDesignW - 2 * kPanelX;   // 1256
constexpr int kPanelH = kTabBodyH - 2 * kPanelY;  // 389
constexpr int kRowPitch = 72;
constexpr int kBarX = 480;
constexpr int kBarW = 700;
}  // namespace

int UIVoicePage::paramsForStage(Stage s, Param* out, int max) const {
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
            add("TRACK", kParamSlot, 0, "");
            add("PITCH", WaveX::Protocol::PARAM_PITCH, 32768, "semi");
            add("PAN", WaveX::Protocol::PARAM_PAN, 32768, "");
            add("GAIN", WaveX::Protocol::PARAM_VOLUME, 52428, "");
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
            add("LEVEL", WaveX::Protocol::PARAM_VOLUME, 52428, "");
            add("ENV->AMP", kParamNone, 65535, "");
            break;
        case Stage::Filter:
            add("CUTOFF", WaveX::Protocol::PARAM_FILTER_CUTOFF, 65535, "");
            add("RES", WaveX::Protocol::PARAM_FILTER_RESONANCE, 0, "");
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
void UIVoicePage::seedValues() {
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
uint8_t UIVoicePage::currentSlot() const {
    return getCurrentTrack();
}

// Steps sample_id_ to the next/previous resident sample (wrapping) and
// re-binds it to the selected Track - the Sample tab's "which sample this
// Instrument plays" control (roadmap Phase 2.5 item 1, "retire the
// fallback"). There is no "list of loaded ids" query, so this probes
// inter_mcu_get_sample_meta() over the same id range the Sample Manager
// page's list does, rather than inventing a second source of truth for it.
void UIVoicePage::cycleSample(int direction) {
    constexpr int32_t kMaxProbeId = 64;
    for (int32_t step = 1; step <= kMaxProbeId; ++step) {
        int32_t candidate = static_cast<int32_t>(sample_id_) + direction * step;
        // Wrap into 1..kMaxProbeId (0 is reserved for "no sample").
        candidate = ((candidate - 1) % kMaxProbeId + kMaxProbeId) % kMaxProbeId + 1;
        WaveX::Protocol::SampleMetadata m;
        if (inter_mcu_get_sample_meta(static_cast<uint16_t>(candidate), &m)) {
            sample_id_ = static_cast<uint16_t>(candidate);
            inter_mcu_send_sample_select(sample_id_, currentSlot());
            refreshHeader();
            refreshParams();
            return;
        }
    }
    refreshStatus("No other samples resident - load one from Browse");
}

void UIVoicePage::onEnter(lv_obj_t* parent) {
    lv_obj_clean(parent);
    stage_ = 0;
    param_ = 0;
    editing_ = false;
    for (auto& b: stage_built_) {
        b = false;
    }
    for (int s = 0; s < kStageCount; ++s) {
        for (int i = 0; i < kMaxParams; ++i) {
            param_rows_[s][i] = nullptr;
            param_bars_[s][i] = nullptr;
        }
    }

    root_ = lv_obj_create(parent);
    lv_obj_set_size(root_, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(root_, lv_color_hex(kColBg), LV_PART_MAIN);
    lv_obj_set_style_border_width(root_, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(root_, 0, LV_PART_MAIN);
    lv_obj_remove_flag(root_, LV_OBJ_FLAG_SCROLLABLE);

    buildStrip(root_);

    // Tabview sits below the strip and takes the rest.
    lv_obj_t* tab_host = lv_obj_create(root_);
    lv_obj_set_size(tab_host, lv_pct(100), kContentH - kStripH);
    lv_obj_set_pos(tab_host, 0, kStripH);
    lv_obj_set_style_bg_color(tab_host, lv_color_hex(kColBg), LV_PART_MAIN);
    lv_obj_set_style_border_width(tab_host, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(tab_host, 0, LV_PART_MAIN);
    lv_obj_remove_flag(tab_host, LV_OBJ_FLAG_SCROLLABLE);

    tabview_ = tabGroupCreate(tab_host);
    for (int s = 0; s < kStageCount; ++s) {
        tab_body_[s] = tabGroupAddTab(tabview_, kStageNames[s]);
    }
    lv_obj_add_event_cb(tabview_, &UIVoicePage::tabChangedCb, LV_EVENT_VALUE_CHANGED, this);

    // The Instrument edits whatever sample is selected. Asking for metadata means
    // the header can name it rather than showing a bare id.
    seedValues();

    sample_id_ = 0;
    if (auto* state = getSampleBrowserState()) {
        sample_id_ = state->last_load_sample_id;
    }
    if (sample_id_ != 0) {
        inter_mcu_request_sample_meta(sample_id_);
        inter_mcu_send_sample_select(sample_id_, currentSlot());
    }

    // Only the first tab's widgets exist after this; the rest are built when
    // first shown.
    buildStageRows(stage_);
    refreshHeader();
    refreshParams();
}

void UIVoicePage::onExit() {
    if (root_) {
        lv_obj_del(root_);
        root_ = nullptr;
        name_label_ = nullptr;
        status_label_ = nullptr;
        tabview_ = nullptr;
        for (int s = 0; s < kStageCount; ++s) {
            tab_body_[s] = nullptr;
            stage_built_[s] = false;
            for (int i = 0; i < kMaxParams; ++i) {
                param_rows_[s][i] = nullptr;
                param_bars_[s][i] = nullptr;
            }
        }
    }
}

// The Instrument-scoped strip: which Instrument, Track and sample on the first line, the last
// thing the page had to say on the second. Above the tabview rather than in a
// tab body, so switching stage does not hide it (see the class note).
void UIVoicePage::buildStrip(lv_obj_t* parent) {
    lv_obj_t* strip = lv_obj_create(parent);
    lv_obj_set_size(strip, lv_pct(100), kStripH);
    lv_obj_set_pos(strip, 0, 0);
    lv_obj_set_style_bg_color(strip, lv_color_hex(kColBg), LV_PART_MAIN);
    lv_obj_set_style_border_width(strip, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(strip, 0, LV_PART_MAIN);
    lv_obj_remove_flag(strip, LV_OBJ_FLAG_SCROLLABLE);

    name_label_ = lv_label_create(strip);
    lv_obj_set_style_text_font(name_label_, &lv_font_montserrat_22, LV_PART_MAIN);
    lv_obj_set_width(name_label_, kPanelW);
    lv_label_set_long_mode(name_label_, LV_LABEL_LONG_DOT);
    lv_obj_set_pos(name_label_, kPanelX, 8);
    lv_label_set_text(name_label_, "");

    status_label_ = lv_label_create(strip);
    lv_obj_set_style_text_font(status_label_, &lv_font_montserrat_18, LV_PART_MAIN);
    lv_obj_set_style_text_color(status_label_, lv_color_hex(kColDim), LV_PART_MAIN);
    lv_obj_set_width(status_label_, kPanelW);
    lv_label_set_long_mode(status_label_, LV_LABEL_LONG_DOT);
    lv_obj_set_pos(status_label_, kPanelX, 46);
    lv_label_set_text(status_label_, "");
}

// Builds one tab's parameter rows. Called on first display of that tab, not up
// front: entering the page then costs three rows rather than seventeen, and
// page entry is what this UI pays for (docs/backlog.md).
void UIVoicePage::buildStageRows(int stage) {
    if (stage < 0 || stage >= kStageCount || stage_built_[stage] || !tab_body_[stage]) {
        return;
    }

    // Shared styles, not per-object local ones.
    //
    // Every lv_obj_set_style_*() call stores a property in the object's OWN
    // style list, which allocates. What genuinely varies per row is the label
    // text and its colour; the panel chrome, row font and bar fills are
    // identical everywhere, so they belong in one style each row references.
    //
    // Function-local statics: initialised once, never destroyed, which is what
    // an lv_style_t referenced by live objects requires. UI task only, so the
    // one-time init needs no locking beyond what C++ already guarantees.
    static lv_style_t s_panel;
    static lv_style_t s_row;
    static lv_style_t s_bar_main;
    static lv_style_t s_bar_ind;
    static bool s_styles_ready = false;
    if (!s_styles_ready) {
        lv_style_init(&s_panel);
        // remove_style_all() takes the theme's opaque background with it, so
        // the base style has to restore the parts a panel actually needs.
        lv_style_set_bg_opa(&s_panel, LV_OPA_COVER);
        lv_style_set_bg_color(&s_panel, lv_color_hex(kColCard));
        lv_style_set_border_width(&s_panel, 1);
        lv_style_set_border_color(&s_panel, lv_color_hex(kColBorder));
        lv_style_set_radius(&s_panel, 4);

        lv_style_init(&s_row);
        lv_style_set_text_font(&s_row, &lv_font_montserrat_22);

        lv_style_init(&s_bar_main);
        lv_style_set_bg_opa(&s_bar_main, LV_OPA_COVER);
        lv_style_set_bg_color(&s_bar_main, lv_color_hex(kColTrack));
        lv_style_set_radius(&s_bar_main, 5);

        lv_style_init(&s_bar_ind);
        lv_style_set_bg_opa(&s_bar_ind, LV_OPA_COVER);
        lv_style_set_bg_color(&s_bar_ind, lv_color_hex(kColGreen));
        lv_style_set_radius(&s_bar_ind, 5);

        s_styles_ready = true;
    }

    lv_obj_t* panel = lv_obj_create(tab_body_[stage]);
    // Drop the theme's default styling before adding ours; none of it survives
    // visually, so applying it and then overriding it is pure page-entry cost.
    lv_obj_remove_style_all(panel);
    lv_obj_add_style(panel, &s_panel, LV_PART_MAIN);
    lv_obj_set_size(panel, kPanelW, kPanelH);
    lv_obj_set_pos(panel, kPanelX, kPanelY);
    lv_obj_remove_flag(panel, LV_OBJ_FLAG_SCROLLABLE);

    Param params[kMaxParams];
    const int n = paramsForStage(static_cast<Stage>(stage), params, kMaxParams);
    for (int i = 0; i < n; ++i) {
        lv_obj_t* row = lv_label_create(panel);
        lv_obj_remove_style_all(row);
        lv_obj_add_style(row, &s_row, LV_PART_MAIN);
        lv_obj_set_pos(row, 16, 18 + i * kRowPitch);
        lv_label_set_text(row, "");
        param_rows_[stage][i] = row;

        lv_obj_t* bar = lv_bar_create(panel);
        lv_obj_remove_style_all(bar);
        lv_obj_add_style(bar, &s_bar_main, LV_PART_MAIN);
        lv_obj_add_style(bar, &s_bar_ind, LV_PART_INDICATOR);
        lv_obj_set_size(bar, kBarW, 10);
        lv_obj_set_pos(bar, kBarX, 32 + i * kRowPitch);
        lv_bar_set_range(bar, 0, 65535);
        lv_bar_set_value(bar, 0, LV_ANIM_OFF);
        lv_obj_add_flag(bar, LV_OBJ_FLAG_HIDDEN);
        param_bars_[stage][i] = bar;
    }

    stage_built_[stage] = true;
}

void UIVoicePage::tabChangedCb(lv_event_t* e) {
    auto* self = static_cast<UIVoicePage*>(lv_event_get_user_data(e));
    if (!self || !self->tabview_) {
        return;
    }
    self->selectStage(static_cast<int>(lv_tabview_get_tab_active(self->tabview_)));
}

// Single entry point for "the visible stage is now this one", whether the user
// tapped the bar or the softkeys/encoder drove it. moveStage() sets the active
// tab, which re-enters here through the event callback, so this has to be a
// no-op when the stage has not actually changed.
void UIVoicePage::selectStage(int stage) {
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

void UIVoicePage::refreshHeader() {
    if (!name_label_ || !lv_obj_is_valid(name_label_)) {
        return;
    }
    char header[128];
    WaveX::Protocol::SampleMetadata m;
    const unsigned slot = trackDisplayNumber(currentSlot());
    if (sample_id_ != 0 && inter_mcu_get_sample_meta(sample_id_, &m)) {
        snprintf(header,
                 sizeof(header),
                 "%s   -   Track %u   sample %u  %.32s",
                 voice_name_,
                 slot,
                 (unsigned)sample_id_,
                 m.name);
    } else if (sample_id_ != 0) {
        snprintf(header,
                 sizeof(header),
                 "%s   -   Track %u   sample %u",
                 voice_name_,
                 slot,
                 (unsigned)sample_id_);
    } else {
        snprintf(header,
                 sizeof(header),
                 "%s   -   Track %u   no sample (load one from Sample > Browse)",
                 voice_name_,
                 slot);
    }
    lv_label_set_text(name_label_, header);
}

void UIVoicePage::refreshParams() {
    Param params[kMaxParams];
    const int n = paramsForStage(static_cast<Stage>(stage_), params, kMaxParams);
    if (param_ >= n) {
        param_ = n > 0 ? n - 1 : 0;
    }

    for (int i = 0; i < n; ++i) {
        lv_obj_t* row = param_rows_[stage_][i];
        if (!row || !lv_obj_is_valid(row)) {
            continue;
        }

        const Param& p = params[i];
        const bool inert = (p.wire_param == kParamNone);
        // Discrete choices, not continuous CC values - shown as text, no bar.
        const bool discrete = (p.wire_param == kParamSample || p.wire_param == kParamSlot);
        const bool focused = (i == param_);

        char line[96];
        if (inert) {
            snprintf(line, sizeof(line), "%s%-10s  --  not wired", focused ? "> " : "  ", p.label);
        } else if (p.wire_param == kParamSample) {
            WaveX::Protocol::SampleMetadata m;
            if (sample_id_ == 0) {
                snprintf(
                    line, sizeof(line), "%s%-10s  none loaded", focused ? "> " : "  ", p.label);
            } else if (inter_mcu_get_sample_meta(sample_id_, &m)) {
                snprintf(line,
                         sizeof(line),
                         "%s%-10s  %u %.24s",
                         focused ? "> " : "  ",
                         p.label,
                         (unsigned)sample_id_,
                         m.name);
            } else {
                snprintf(line,
                         sizeof(line),
                         "%s%-10s  %u",
                         focused ? "> " : "  ",
                         p.label,
                         (unsigned)sample_id_);
            }
        } else if (p.wire_param == kParamSlot) {
            snprintf(line,
                     sizeof(line),
                     "%s%-10s  %u",
                     focused ? "> " : "  ",
                     p.label,
                     trackDisplayNumber(getCurrentTrack()));
        } else {
            snprintf(line,
                     sizeof(line),
                     "%s%-10s  %5u %s",
                     focused ? "> " : "  ",
                     p.label,
                     (unsigned)p.value,
                     p.unit);
        }
        lv_label_set_text(row, line);
        lv_obj_set_style_text_color(
            row, lv_color_hex(inert ? kColInert : (focused ? kColGreen : 0xFFFFFF)), LV_PART_MAIN);

        lv_obj_t* bar = param_bars_[stage_][i];
        if (bar && lv_obj_is_valid(bar)) {
            if (inert || discrete) {
                lv_obj_add_flag(bar, LV_OBJ_FLAG_HIDDEN);
            } else {
                lv_obj_remove_flag(bar, LV_OBJ_FLAG_HIDDEN);
                lv_bar_set_value(bar, p.value, LV_ANIM_OFF);
            }
        }
    }
}

void UIVoicePage::refreshStatus(const char* text) {
    if (status_label_ && lv_obj_is_valid(status_label_)) {
        lv_label_set_text(status_label_, text ? text : "");
    }
}

void UIVoicePage::sendParam(const Param& p) {
    if (p.wire_param == kParamNone) {
        refreshStatus("Not wired: the protocol carries no modulation routing yet");
        return;
    }
    if (p.wire_param == kParamSample || p.wire_param == kParamSlot) {
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

void UIVoicePage::stepParam(int steps) {
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
    if (p.wire_param == kParamSlot) {
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
void UIVoicePage::moveStage(int delta) {
    const int next = (stage_ + delta + kStageCount) % kStageCount;
    if (tabview_ && lv_obj_is_valid(tabview_)) {
        lv_tabview_set_active(tabview_, static_cast<uint32_t>(next), LV_ANIM_OFF);
    }
    // lv_tabview_set_active() fires LV_EVENT_VALUE_CHANGED, so selectStage()
    // has usually already run by here; calling it again is the no-op guard's
    // job and covers the case where the tabview is gone.
    selectStage(next);
}

void UIVoicePage::moveParam(int delta) {
    Param params[kMaxParams];
    const int n = paramsForStage(static_cast<Stage>(stage_), params, kMaxParams);
    if (n == 0) {
        return;
    }
    param_ = (param_ + delta + n) % n;
    refreshParams();
}

void UIVoicePage::onInput(const InputEvent& evt) {
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
        case InputType::EncoderDown:
            moveStage(+1);
            break;
        case InputType::EncoderUp:
            moveStage(-1);
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

std::array<Softkey, NUM_SOFTKEYS> UIVoicePage::getSoftkeys() {
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

std::array<Softkey, NUM_SOFTKEYS> UIVoicePage::getShiftedSoftkeys() {
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
                   snprintf(voice_name_, sizeof(voice_name_), "Init Instrument");
                   refreshHeader();
                   refreshParams();
                   refreshStatus("Instrument reset to defaults");
               }};
    return keys;
}

std::shared_ptr<UIPage> createVoicePage() {
    return std::make_shared<UIVoicePage>();
}

}  // namespace wavex_ui
