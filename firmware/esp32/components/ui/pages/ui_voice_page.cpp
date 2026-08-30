// WaveX Voice / Preset editor
#include "ui/ui_voice_page.h"

#include <esp_log.h>

#include "../styles/ui_theme.h"
#include "inter_mcu.h"
#include "ui/ui_card.h"
#include "ui/ui_navigator.h"
#include "ui/ui_sample_browser.h"

#include <cstdio>
#include <cstring>

namespace wavex_ui {

namespace {
static const char* TAG = "UI_VOICE";

constexpr uint32_t kColPanel = 0x0E0E0E;
constexpr uint32_t kColBorder = 0x222222;
constexpr uint32_t kColDim = 0x8FA0AA;
constexpr uint32_t kColGreen = 0x4CAF50;
constexpr uint32_t kColInert = 0x5A5A5A;

// 64 detents end to end: fine enough to sound continuous, coarse enough to
// cross the range without grinding. Matches the keyboard page's feel.
constexpr int kParamStep = 65535 / 64;

const char* const kStageNames[] = {"SAMPLE", "MOD", "ENV", "AMP", "FILTER"};

// Chain geometry. One row of stage tiles across the top, in signal order, so
// the page reads as the path the audio takes rather than as a form.
constexpr int kTileW = 236;
constexpr int kTileH = 74;
constexpr int kTilePitch = 250;
constexpr int kTileY = 8;
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
            // Live: the engine applies all three to sounding voices.
            add("PITCH", WaveX::Protocol::PARAM_PITCH, 32768, "semi");
            add("PAN", WaveX::Protocol::PARAM_PAN, 32768, "");
            add("GAIN", WaveX::Protocol::PARAM_VOLUME, 52428, "");
            break;
        case Stage::Mod:
            // Inert, and labelled so. Nothing on the wire carries a modulation
            // source, destination or depth yet; drawing a matrix that silently
            // does nothing is the failure this codebase has already had once.
            add("SOURCE", kParamNone, 0, "");
            add("DEST", kParamNone, 0, "");
            add("DEPTH", kParamNone, 0, "");
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
        Param params[6];
        const int n = paramsForStage(static_cast<Stage>(s), params, 6);
        for (int i = 0; i < n; ++i) {
            stage_values_[s][i] = params[i].value;
        }
    }
    values_seeded_ = true;
}

void UIVoicePage::onEnter(lv_obj_t* parent) {
    lv_obj_clean(parent);

    root_ = lv_obj_create(parent);
    lv_obj_set_size(root_, lv_pct(100), lv_pct(100));
    ui_theme_apply_container_style(root_, true);
    lv_obj_set_style_pad_all(root_, 0, LV_PART_MAIN);
    lv_obj_remove_flag(root_, LV_OBJ_FLAG_SCROLLABLE);

    name_label_ = lv_label_create(root_);
    ui_theme_apply_label_style(name_label_, false);
    lv_obj_set_pos(name_label_, 12, kTileY + kTileH + 14);

    buildChain(root_);
    buildParamPanel(root_);

    status_label_ = lv_label_create(root_);
    ui_theme_apply_label_style(status_label_, false);
    lv_obj_set_style_text_color(status_label_, lv_color_hex(kColDim), LV_PART_MAIN);
    lv_obj_set_width(status_label_, 1240);
    lv_label_set_long_mode(status_label_, LV_LABEL_LONG_DOT);
    lv_obj_set_pos(status_label_, 12, 500);
    lv_label_set_text(status_label_, "");

    // The voice edits whatever sample is selected. Asking for metadata means
    // the header can name it rather than showing a bare id.
    seedValues();

    sample_id_ = 0;
    if (auto* state = getSampleBrowserState()) {
        sample_id_ = state->last_load_sample_id;
    }
    if (sample_id_ != 0) {
        inter_mcu_request_sample_meta(sample_id_);
        inter_mcu_send_sample_select(sample_id_);
    }

    refreshChain();
    refreshParams();
}

void UIVoicePage::onExit() {
    if (root_) {
        lv_obj_del(root_);
        root_ = nullptr;
        name_label_ = nullptr;
        param_panel_ = nullptr;
        status_label_ = nullptr;
        for (auto& c: chain_) {
            c = nullptr;
        }
        for (auto& c: chain_label_) {
            c = nullptr;
        }
        for (auto& r: param_rows_) {
            r = nullptr;
        }
        for (auto& b: param_bars_) {
            b = nullptr;
        }
    }
}

void UIVoicePage::buildChain(lv_obj_t* parent) {
    for (int i = 0; i < kStageCount; ++i) {
        lv_obj_t* tile = lv_obj_create(parent);
        lv_obj_set_size(tile, kTileW, kTileH);
        lv_obj_set_pos(tile, 12 + i * kTilePitch, kTileY);
        lv_obj_set_style_bg_color(tile, lv_color_hex(kColPanel), LV_PART_MAIN);
        lv_obj_set_style_border_width(tile, 1, LV_PART_MAIN);
        lv_obj_set_style_border_color(tile, lv_color_hex(kColBorder), LV_PART_MAIN);
        lv_obj_set_style_radius(tile, 4, LV_PART_MAIN);
        lv_obj_remove_flag(tile, LV_OBJ_FLAG_SCROLLABLE);
        cardApplyDropShadow(tile);

        lv_obj_t* label = lv_label_create(tile);
        lv_obj_set_style_text_font(label, &lv_font_montserrat_22, LV_PART_MAIN);
        lv_label_set_text(label, kStageNames[i]);
        lv_obj_center(label);

        chain_[i] = tile;
        chain_label_[i] = label;
    }
}

void UIVoicePage::buildParamPanel(lv_obj_t* parent) {
    param_panel_ = lv_obj_create(parent);
    lv_obj_set_size(param_panel_, 1256, 300);
    lv_obj_set_pos(param_panel_, 12, kTileY + kTileH + 46);
    lv_obj_set_style_bg_color(param_panel_, lv_color_hex(kColPanel), LV_PART_MAIN);
    lv_obj_set_style_border_width(param_panel_, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(param_panel_, lv_color_hex(kColBorder), LV_PART_MAIN);
    lv_obj_remove_flag(param_panel_, LV_OBJ_FLAG_SCROLLABLE);
    cardApplyDropShadow(param_panel_);

    for (int i = 0; i < 6; ++i) {
        lv_obj_t* row = lv_label_create(param_panel_);
        ui_theme_apply_label_style(row, false);
        lv_obj_set_pos(row, 16, 14 + i * 46);
        lv_label_set_text(row, "");
        param_rows_[i] = row;

        lv_obj_t* bar = lv_bar_create(param_panel_);
        lv_obj_set_size(bar, 700, 10);
        lv_obj_set_pos(bar, 480, 26 + i * 46);
        lv_bar_set_range(bar, 0, 65535);
        lv_bar_set_value(bar, 0, LV_ANIM_OFF);
        lv_obj_set_style_bg_color(bar, lv_color_hex(0x1F1F1F), LV_PART_MAIN);
        lv_obj_set_style_bg_color(bar, lv_color_hex(kColGreen), LV_PART_INDICATOR);
        lv_obj_add_flag(bar, LV_OBJ_FLAG_HIDDEN);
        param_bars_[i] = bar;
    }
}

void UIVoicePage::refreshChain() {
    for (int i = 0; i < kStageCount; ++i) {
        if (!chain_[i] || !lv_obj_is_valid(chain_[i])) {
            continue;
        }
        const bool focused = (i == stage_);
        lv_obj_set_style_border_color(
            chain_[i], lv_color_hex(focused ? kColGreen : kColBorder), LV_PART_MAIN);
        lv_obj_set_style_border_width(chain_[i], focused ? 2 : 1, LV_PART_MAIN);
        // MOD reads dim because it is not wired, not because it is unfocused.
        const bool inert = (i == static_cast<int>(Stage::Mod));
        lv_obj_set_style_text_color(
            chain_label_[i], lv_color_hex(inert ? kColInert : 0xFFFFFF), LV_PART_MAIN);
    }

    if (name_label_) {
        char header[128];
        WaveX::Protocol::SampleMetadata m;
        if (sample_id_ != 0 && inter_mcu_get_sample_meta(sample_id_, &m)) {
            snprintf(header,
                     sizeof(header),
                     "%s   -   sample %u  %.32s",
                     voice_name_,
                     (unsigned)sample_id_,
                     m.name);
        } else if (sample_id_ != 0) {
            snprintf(
                header, sizeof(header), "%s   -   sample %u", voice_name_, (unsigned)sample_id_);
        } else {
            snprintf(
                header, sizeof(header), "%s   -   no sample (load one from Browse)", voice_name_);
        }
        lv_label_set_text(name_label_, header);
    }
}

void UIVoicePage::refreshParams() {
    Param params[6];
    const int n = paramsForStage(static_cast<Stage>(stage_), params, 6);
    if (param_ >= n) {
        param_ = n > 0 ? n - 1 : 0;
    }

    for (int i = 0; i < 6; ++i) {
        if (!param_rows_[i] || !lv_obj_is_valid(param_rows_[i])) {
            continue;
        }
        if (i >= n) {
            lv_label_set_text(param_rows_[i], "");
            if (param_bars_[i]) {
                lv_obj_add_flag(param_bars_[i], LV_OBJ_FLAG_HIDDEN);
            }
            continue;
        }

        const Param& p = params[i];
        const bool inert = (p.wire_param == kParamNone);
        const bool focused = (i == param_);

        char line[96];
        if (inert) {
            snprintf(line, sizeof(line), "%s%-10s  --  not wired", focused ? "> " : "  ", p.label);
        } else {
            snprintf(line,
                     sizeof(line),
                     "%s%-10s  %5u %s",
                     focused ? "> " : "  ",
                     p.label,
                     (unsigned)p.value,
                     p.unit);
        }
        lv_label_set_text(param_rows_[i], line);
        lv_obj_set_style_text_color(
            param_rows_[i],
            lv_color_hex(inert ? kColInert : (focused ? kColGreen : 0xFFFFFF)),
            LV_PART_MAIN);

        if (param_bars_[i]) {
            if (inert) {
                lv_obj_add_flag(param_bars_[i], LV_OBJ_FLAG_HIDDEN);
            } else {
                lv_obj_remove_flag(param_bars_[i], LV_OBJ_FLAG_HIDDEN);
                lv_bar_set_value(param_bars_[i], p.value, LV_ANIM_OFF);
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
    if (inter_mcu_send_control_change(p.wire_param, 0, p.value) != ESP_OK) {
        refreshStatus("Send failed - link busy?");
    }
}

void UIVoicePage::stepParam(int steps) {
    Param params[6];
    const int n = paramsForStage(static_cast<Stage>(stage_), params, 6);
    if (param_ < 0 || param_ >= n) {
        return;
    }
    Param& p = params[param_];
    if (p.wire_param == kParamNone) {
        refreshStatus("Not wired: the protocol carries no modulation routing yet");
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

void UIVoicePage::moveStage(int delta) {
    stage_ = (stage_ + delta + kStageCount) % kStageCount;
    param_ = 0;
    refreshChain();
    refreshParams();
    UINavigator::instance().refreshSoftkeys();
}

void UIVoicePage::moveParam(int delta) {
    Param params[6];
    const int n = paramsForStage(static_cast<Stage>(stage_), params, 6);
    if (n == 0) {
        return;
    }
    param_ = (param_ + delta + n) % n;
    refreshParams();
}

void UIVoicePage::onInput(const InputEvent& evt) {
    switch (evt.type) {
        case InputType::EncoderRight:
            if (editing_) {
                stepParam(evt.delta ? evt.delta : 1);
            } else {
                moveParam(+1);
            }
            break;
        case InputType::EncoderLeft:
            if (editing_) {
                stepParam(-(evt.delta ? evt.delta : 1));
            } else {
                moveParam(-1);
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
    keys[1] = {"< Stage", [this]() { moveStage(-1); }};
    keys[2] = {"Stage >", [this]() { moveStage(+1); }};
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
    // Save/Load are shown but unwired, and say why: a preset needs an on-disk
    // format and protocol messages that do not exist. Better a labelled gap
    // than a button that appears to work.
    keys[1] = {"Save", nullptr, false, "needs a preset format on disk"};
    keys[2] = {"Save As", nullptr, false, "needs filename entry"};
    keys[3] = {"Load", nullptr, false, "needs a preset format on disk"};
    keys[4] = {"Init", [this]() {
                   // Re-send every wired parameter at its default so the engine
                   // and the page agree again.
                   for (int s = 0; s < kStageCount; ++s) {
                       Param params[6];
                       const int n = paramsForStage(static_cast<Stage>(s), params, 6);
                       for (int i = 0; i < n; ++i) {
                           stage_values_[s][i] = params[i].value;
                           if (params[i].wire_param != kParamNone) {
                               sendParam(params[i]);
                           }
                       }
                   }
                   snprintf(voice_name_, sizeof(voice_name_), "Init Voice");
                   refreshChain();
                   refreshParams();
                   refreshStatus("Voice reset to defaults");
               }};
    return keys;
}

std::shared_ptr<UIPage> createVoicePage() {
    return std::make_shared<UIVoicePage>();
}

}  // namespace wavex_ui
