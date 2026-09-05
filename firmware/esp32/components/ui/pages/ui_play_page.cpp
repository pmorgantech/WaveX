// WaveX Play page - Pads and Keys performance surfaces
#include "ui/ui_play_page.h"

#include <esp_log.h>

#include "../styles/ui_theme.h"
#include "inter_mcu.h"
#include "spi_protocol/protocol.h"
#include "ui/current_track.h"
#include "ui/ui_navigator.h"
#include "ui/ui_palette.h"
#include "ui/ui_tab_group.h"

#include <cmath>
#include <cstdio>

namespace wavex_ui {

using namespace wavex_ui::palette;

namespace {

constexpr int kMidiNoteMax = 127;

// Content geometry. The panel is 720x1280 rotated to 1280x720; the navigator
// takes UI_HEADER_HEIGHT (75) off the top and UI_HOTKEY_HEIGHT (100) off the
// bottom, and the tab bar takes 56 more. Positions are absolute against that,
// matching how the diagnostics cards are laid out rather than introducing a
// second convention.
constexpr int kDesignW = 1280;
constexpr int kContentH = 720 - UI_HEADER_HEIGHT - UI_HOTKEY_HEIGHT;  // 545
// The status/parameter strip sits ABOVE the tabview, not inside a tab. It has
// to: the parameters are page-scoped, and a strip built into one tab body would
// vanish when the other tab was selected - taking the only readout of what the
// encoder is editing with it.
constexpr int kStripH = 56;
constexpr int kTabBodyH = kContentH - kStripH - 56;  // 433, after the tab bar
constexpr int kKeysY = 0;
constexpr int kKeysH = kTabBodyH;

// Piano: two octaves plus the closing C. 15 white keys, 10 black.
constexpr int kWhitePerOctave = 7;
constexpr int kWhiteCount = kWhitePerOctave * 2 + 1;
constexpr int8_t kWhiteOffset[kWhitePerOctave] = {0, 2, 4, 5, 7, 9, 11};
// Black keys sit above the gap after these white indices within an octave.
constexpr int kBlackAfter[5] = {0, 1, 3, 4, 5};
constexpr int kWhiteW = kDesignW / kWhiteCount;  // 85
constexpr int kBlackW = (kWhiteW * 3) / 5;       // 51
constexpr int kBlackH = (kKeysH * 3) / 5;

// Piano key fills. Not in the shared palette: they are specific to this
// instrument, and a piano that is not black-and-white stops reading as one.
constexpr uint32_t kColWhiteKey = 0xE8E8E8;
constexpr uint32_t kColBlackKey = 0x101010;

// 64 detents end to end: fine enough that a sweep sounds continuous, coarse
// enough to cross the range without grinding.
constexpr int kParamStep = 65535 / 64;

const char* const kNoteNames[12] = {
    "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};

// Scientific pitch notation, where MIDI 60 is C4.
void NoteName(int note, char* out, size_t len) {
    if (note < 0 || note > kMidiNoteMax) {
        snprintf(out, len, "--");
        return;
    }
    snprintf(out, len, "%s%d", kNoteNames[note % 12], (note / 12) - 1);
}

struct ParamSpec {
    const char* label;
    uint8_t wire_param;
    uint16_t initial;
};

// Initial values mirror the engine's VoiceLiveParams defaults, so opening this
// page does not silently change the sound before anything is touched. Cutoff
// 65535 is 20 kHz under the engine's exponential map - effectively open.
const ParamSpec kParams[static_cast<size_t>(UIPlayPage::Param::kCount)] = {
    {"CUTOFF", WaveX::Protocol::PARAM_FILTER_CUTOFF, 65535},
    {"RES", WaveX::Protocol::PARAM_FILTER_RESONANCE, 0},
    {"ATTACK", WaveX::Protocol::PARAM_ENVELOPE_ATTACK, 0},
    {"DECAY", WaveX::Protocol::PARAM_ENVELOPE_DECAY, 1638},
    {"SUSTAIN", WaveX::Protocol::PARAM_ENVELOPE_SUSTAIN, 52428},
    {"RELEASE", WaveX::Protocol::PARAM_ENVELOPE_RELEASE, 3277},
    // wire_param is unused for Track - stepParam()/sendParam() special-case it
    // rather than sending MSG_CONTROL_CHANGE, since it addresses which
    // Track a note-on goes out on, not a voice parameter value.
    {"TRACK", 0, 0},
};

// Mirrors the Daisy's own mapping so the number on screen is the number the
// engine used, not a second opinion about it. Keep in step with
// audio_engine.cpp's OnControlChange.
void FormatParamValue(UIPlayPage::Param p, uint16_t raw, char* out, size_t len) {
    const float norm = static_cast<float>(raw) / 65535.0f;
    switch (p) {
        case UIPlayPage::Param::Cutoff:
            snprintf(out, len, "%d Hz", (int)(20.0f * powf(1000.0f, norm)));
            break;
        case UIPlayPage::Param::Resonance:
        case UIPlayPage::Param::Sustain:
            snprintf(out, len, "%d%%", (int)(norm * 100.0f + 0.5f));
            break;
        case UIPlayPage::Param::Track:
            // Not a CC value - raw IS the 0-based Track index, shown 1-based
            // like every other Track on screen.
            snprintf(out, len, "%u", trackDisplayNumber(static_cast<uint8_t>(raw)));
            break;
        default:  // envelope times: 1 ms .. 2 s
            snprintf(out, len, "%d ms", (int)((0.001f + norm * 2.0f) * 1000.0f));
            break;
    }
}

}  // namespace

// --- lifecycle -------------------------------------------------------------

void UIPlayPage::onEnter(lv_obj_t* parent) {
    lv_obj_clean(parent);
    key_count_ = 0;
    for (auto& k: keys_) {
        k = Key{};
    }
    for (size_t i = 0; i < static_cast<size_t>(Param::kCount); ++i) {
        param_value_[i] = kParams[i].initial;
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
    lv_obj_t* t_pads = tabGroupAddTab(tabview_, "Pads");
    lv_obj_t* t_keys = tabGroupAddTab(tabview_, "Keys");

    buildPads(t_pads);
    buildKeys(t_keys);

    // Switching tabs releases everything. A latched note whose key is on the
    // other tab is a note you cannot see and will not think to stop - the same
    // reason latched keys are drawn lit.
    lv_obj_add_event_cb(tabview_, &UIPlayPage::tabChangedCb, LV_EVENT_VALUE_CHANGED, this);

    refreshKeys();
    refreshParamLabel();
    // The binding lives on the Daisy (and may be an SFZ Instrument whose samples
    // are intentionally absent from the frontend's bare-sample metadata).
    // Request it here and refresh it lightly while the page is open so a Load
    // or Select performed elsewhere cannot leave a stale claim on Play.
    inter_mcu_request_track_binding(currentTrack());
    binding_timer_ = lv_timer_create(bindingTimerCb, 500, this);
}

void UIPlayPage::onExit() {
    // Leaving with a key down would strand a Note On with no matching Note Off,
    // and the voice would sustain until something stole it.
    releaseAll();
    if (binding_timer_) {
        lv_timer_delete(binding_timer_);
        binding_timer_ = nullptr;
    }
    if (root_) {
        lv_obj_del(root_);
        root_ = nullptr;
        tabview_ = nullptr;
        status_label_ = nullptr;
        param_label_ = nullptr;
    }
    key_count_ = 0;
    for (auto& k: keys_) {
        k = Key{};
    }
}

void UIPlayPage::tabChangedCb(lv_event_t* e) {
    auto* self = static_cast<UIPlayPage*>(lv_event_get_user_data(e));
    if (self) {
        self->releaseAll();
        self->refreshKeys();
    }
}

void UIPlayPage::bindingTimerCb(lv_timer_t* timer) {
    auto* self = static_cast<UIPlayPage*>(lv_timer_get_user_data(timer));
    if (!self) {
        return;
    }
    // The request is small and idempotent. The timer is in LVGL context, so
    // the label update is safe; the UART task only fills the shared cache.
    inter_mcu_request_track_binding(self->currentTrack());
    self->refreshBindingStatus();
}

// --- layout ----------------------------------------------------------------

lv_obj_t* UIPlayPage::makeKey(
    lv_obj_t* parent, int8_t offset, bool is_black, uint32_t bg, uint32_t text) {
    if (key_count_ >= kMaxKeys) {
        return nullptr;
    }
    // Shared styles, not per-object local ones.
    //
    // Every lv_obj_set_style_*() call stores a property in the object's OWN
    // style list, which allocates. This page builds 41 keys, each a button
    // plus a label, so the six local properties these used to set were ~500
    // property stores on a single page entry - and page entry is the whole
    // cost of this page (docs/backlog.md). What genuinely varies per key is
    // the two colours; the border, radius and pressed fill are identical
    // across all 41, so they belong in one style every key references.
    //
    // Function-local statics: initialised once, never destroyed, which is what
    // an lv_style_t referenced by live objects requires. UI task only, so the
    // one-time init needs no locking beyond what C++ already guarantees.
    static lv_style_t s_key_base;
    static lv_style_t s_key_pressed;
    static lv_style_t s_label_base;
    static bool s_styles_ready = false;
    if (!s_styles_ready) {
        lv_style_init(&s_key_base);
        // remove_style_all() takes the theme's opaque background with it, so
        // the base style has to restore the parts a key actually needs.
        lv_style_set_bg_opa(&s_key_base, LV_OPA_COVER);
        lv_style_set_border_width(&s_key_base, 1);
        lv_style_set_border_color(&s_key_base, lv_color_hex(kColBorder));
        lv_style_set_radius(&s_key_base, 4);

        lv_style_init(&s_key_pressed);
        lv_style_set_bg_opa(&s_key_pressed, LV_OPA_COVER);
        lv_style_set_bg_color(&s_key_pressed, lv_color_hex(kColGreen));

        lv_style_init(&s_label_base);
        lv_style_set_text_font(&s_label_base, &lv_font_montserrat_18);
        lv_style_set_text_align(&s_label_base, LV_TEXT_ALIGN_CENTER);

        s_styles_ready = true;
    }

    lv_obj_t* btn = lv_btn_create(parent);
    // Drop the theme's default button styling before adding ours. The default
    // theme applies a substantial style to every button it sees - gradients,
    // shadows, transitions, pressed transforms - and this page creates 41 of
    // them in one go. None of it survives our own styling visually, so paying
    // to apply it 41 times and then override it is pure page-entry cost.
    lv_obj_remove_style_all(btn);
    lv_obj_add_style(btn, &s_key_base, LV_PART_MAIN);
    lv_obj_add_style(
        btn, &s_key_pressed, LV_PART_MAIN | static_cast<lv_style_selector_t>(LV_STATE_PRESSED));
    lv_obj_set_style_bg_color(btn, lv_color_hex(bg), LV_PART_MAIN);

    lv_obj_t* label = lv_label_create(btn);
    lv_obj_remove_style_all(label);
    lv_obj_add_style(label, &s_label_base, LV_PART_MAIN);
    lv_obj_set_style_text_color(label, lv_color_hex(text), LV_PART_MAIN);
    lv_obj_align(label, LV_ALIGN_BOTTOM_MID, 0, -4);

    const int index = key_count_++;
    Key& k = keys_[index];
    k.obj = btn;
    k.label = label;
    k.offset = offset;
    k.bg_normal = bg;
    k.text_normal = text;
    k.is_black = is_black;
    k.down = false;

    // A playable key needs press/release, not CLICKED: CLICKED fires on release
    // only, so every note would be zero-length. Gate length follows the finger.
    //
    // PRESS_LOST matters as much as RELEASED - a finger sliding off a key emits
    // it INSTEAD OF RELEASED, so without it that note hangs.
    lv_obj_set_user_data(btn, reinterpret_cast<void*>(static_cast<intptr_t>(index)));
    lv_obj_add_event_cb(btn, keyEventCb, LV_EVENT_PRESSED, this);
    lv_obj_add_event_cb(btn, keyEventCb, LV_EVENT_RELEASED, this);
    lv_obj_add_event_cb(btn, keyEventCb, LV_EVENT_PRESS_LOST, this);
    return btn;
}

// The shared strip: what will sound on the left, what the encoder edits on the
// right. Page-scoped rather than per-tab, so switching surfaces does not hide
// the only readout of the parameter being swept.
void UIPlayPage::buildStrip(lv_obj_t* parent) {
    lv_obj_t* strip = lv_obj_create(parent);
    lv_obj_set_size(strip, lv_pct(100), kStripH);
    lv_obj_set_pos(strip, 0, 0);
    lv_obj_set_style_bg_color(strip, lv_color_hex(kColBg), LV_PART_MAIN);
    lv_obj_set_style_border_width(strip, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(strip, 0, LV_PART_MAIN);
    lv_obj_remove_flag(strip, LV_OBJ_FLAG_SCROLLABLE);

    status_label_ = lv_label_create(strip);
    lv_label_set_long_mode(status_label_, LV_LABEL_LONG_CLIP);
    lv_obj_set_width(status_label_, 900);
    lv_obj_set_style_text_font(status_label_, &lv_font_montserrat_18, LV_PART_MAIN);
    lv_obj_set_style_text_color(status_label_, lv_color_hex(kColDim), LV_PART_MAIN);
    lv_obj_align(status_label_, LV_ALIGN_LEFT_MID, 12, 0);

    param_label_ = lv_label_create(strip);
    lv_obj_set_style_text_font(param_label_, &lv_font_montserrat_22, LV_PART_MAIN);
    lv_obj_set_style_text_color(param_label_, lv_color_hex(kColGreen), LV_PART_MAIN);
    lv_obj_align(param_label_, LV_ALIGN_RIGHT_MID, -12, 0);
}

void UIPlayPage::buildPads(lv_obj_t* tab) {
    const int gap = 8;
    const int cell_w = (kDesignW - gap * (kPadCols + 1)) / kPadCols;
    const int cell_h = (kKeysH - gap * (kPadRows + 1)) / kPadRows;

    for (int r = 0; r < kPadRows; ++r) {
        for (int c = 0; c < kPadCols; ++c) {
            const int8_t offset = static_cast<int8_t>(r * kPadCols + c);
            lv_obj_t* btn = makeKey(tab, offset, false, kColCard, 0xFFFFFF);
            if (!btn) {
                continue;
            }
            lv_obj_set_size(btn, cell_w, cell_h);
            lv_obj_set_pos(btn, gap + c * (cell_w + gap), kKeysY + gap + r * (cell_h + gap));
            lv_obj_align(keys_[key_count_ - 1].label, LV_ALIGN_CENTER, 0, 0);
            lv_obj_set_style_text_font(
                keys_[key_count_ - 1].label, &lv_font_montserrat_26, LV_PART_MAIN);
        }
    }
}

void UIPlayPage::buildKeys(lv_obj_t* tab) {
    // White keys first, then black, because LVGL hit-tests later siblings on
    // top: a black key drawn first would be unreachable wherever it overlaps a
    // white one, which is everywhere.
    const int x0 = (kDesignW - kWhiteW * kWhiteCount) / 2;

    for (int w = 0; w < kWhiteCount; ++w) {
        const int octave = w / kWhitePerOctave;
        const int within = w % kWhitePerOctave;
        const int8_t offset = static_cast<int8_t>(octave * 12 + kWhiteOffset[within]);
        lv_obj_t* btn = makeKey(tab, offset, false, kColWhiteKey, 0x202020);
        if (!btn) {
            continue;
        }
        lv_obj_set_size(btn, kWhiteW - 2, kKeysH - 8);
        lv_obj_set_pos(btn, x0 + w * kWhiteW, kKeysY + 4);
    }

    for (int octave = 0; octave < 2; ++octave) {
        for (int b = 0; b < 5; ++b) {
            const int within = kBlackAfter[b];
            const int8_t offset = static_cast<int8_t>(octave * 12 + kWhiteOffset[within] + 1);
            lv_obj_t* btn = makeKey(tab, offset, true, kColBlackKey, 0xB0B0B0);
            if (!btn) {
                continue;
            }
            // Centred on the seam between this white key and the next.
            const int seam = x0 + (octave * kWhitePerOctave + within + 1) * kWhiteW;
            lv_obj_set_size(btn, kBlackW, kBlackH);
            lv_obj_set_pos(btn, seam - kBlackW / 2, kKeysY + 4);
        }
    }
}

// --- note handling ---------------------------------------------------------

int UIPlayPage::noteFor(const Key& k) const {
    return root_note_ + k.offset;
}

uint8_t UIPlayPage::currentTrack() const {
    // Shared with Sample Manager, Voice and the Browser's SFZ load target, so
    // "which Track?" has one answer across the UI (current_track.h).
    return getCurrentTrack();
}

void UIPlayPage::keyEventCb(lv_event_t* e) {
    auto* self = static_cast<UIPlayPage*>(lv_event_get_user_data(e));
    auto* target = static_cast<lv_obj_t*>(lv_event_get_target(e));
    if (!self || !target) {
        return;
    }
    const int index = static_cast<int>(reinterpret_cast<intptr_t>(lv_obj_get_user_data(target)));
    if (index < 0 || index >= self->key_count_) {
        return;
    }
    switch (lv_event_get_code(e)) {
        case LV_EVENT_PRESSED:
            if (self->latch_) {
                self->toggleLatch(index);
            } else {
                self->press(index);
            }
            break;
        case LV_EVENT_RELEASED:
        case LV_EVENT_PRESS_LOST:
            if (!self->latch_) {
                self->release(index);
            }
            break;
        default:
            break;
    }
}

void UIPlayPage::press(int index) {
    Key& k = keys_[index];
    if (k.down) {
        return;  // already sounding; do not stack Note Ons
    }
    const int note = noteFor(k);
    if (note < 0 || note > kMidiNoteMax) {
        return;
    }
    k.sent_note = static_cast<uint8_t>(note);
    k.down = true;
    inter_mcu_send_note_on_track(k.sent_note, velocity_, currentTrack());
}

void UIPlayPage::release(int index) {
    Key& k = keys_[index];
    if (!k.down) {
        return;  // no matching press (e.g. RELEASED after PRESS_LOST)
    }
    k.down = false;
    inter_mcu_send_note_off_track(k.sent_note, currentTrack());
}

void UIPlayPage::releaseAll() {
    for (int i = 0; i < key_count_; ++i) {
        release(i);
    }
}

// Latched keys sustain until tapped again, which is what makes a one-handed
// sweep possible: the LVGL port is single-touch, so a finger holding a key
// cannot also move a control. The roadmap already settled that latched beats
// held for touch (1.5.2); here held is not merely awkward but impossible.
void UIPlayPage::toggleLatch(int index) {
    if (keys_[index].down) {
        release(index);
    } else {
        press(index);
    }
    refreshKeys();
}

void UIPlayPage::setRoot(int root) {
    const int max_offset = 24;  // the piano's top key
    if (root < 0) {
        root = 0;
    } else if (root + max_offset > kMidiNoteMax) {
        root = kMidiNoteMax - max_offset;
    }
    if (root == root_note_) {
        return;
    }
    // Release first, at the OLD root, so held notes end with the number they
    // were started with.
    releaseAll();
    root_note_ = root;
    refreshKeys();
}

void UIPlayPage::refreshKeys() {
    char name[8];
    for (int i = 0; i < key_count_; ++i) {
        Key& k = keys_[i];
        if (!k.obj || !k.label) {
            continue;
        }
        NoteName(noteFor(k), name, sizeof(name));
        lv_label_set_text(k.label, name);
        // Latched keys stay lit: a sustaining note you cannot see is one you
        // forget about. Resting colour comes from the key itself rather than
        // from working out which surface it belongs to.
        lv_obj_set_style_bg_color(
            k.obj, lv_color_hex(k.down ? kColGreen : k.bg_normal), LV_PART_MAIN);
    }

    refreshBindingStatus();
}

void UIPlayPage::refreshBindingStatus() {
    if (!status_label_) {
        return;
    }

    char low[8], high[8];
    NoteName(root_note_, low, sizeof(low));
    NoteName(root_note_ + kPadCount - 1, high, sizeof(high));

    const uint8_t track = currentTrack();
    WaveX::Protocol::TrackBindingMessage binding;
    char state[128];
    if (!inter_mcu_get_track_binding(track, &binding)) {
        snprintf(state, sizeof(state), "Track %u: checking binding", trackDisplayNumber(track));
    } else {
        switch (binding.state) {
            case WaveX::Protocol::TRACK_BINDING_SAMPLE: {
                WaveX::Protocol::SampleMetadata sample;
                if (inter_mcu_get_sample_meta(binding.sample_id, &sample)) {
                    snprintf(state,
                             sizeof(state),
                             "Track %u: sample %.28s bound (playable)",
                             trackDisplayNumber(track),
                             sample.name);
                } else {
                    snprintf(state,
                             sizeof(state),
                             "Track %u: sample %u bound (playable)",
                             trackDisplayNumber(track),
                             (unsigned)binding.sample_id);
                }
                break;
            }
            case WaveX::Protocol::TRACK_BINDING_PATCH:
                snprintf(state,
                         sizeof(state),
                         "Track %u: Instrument %.23s bound (playable)",
                         trackDisplayNumber(track),
                         binding.name[0] ? binding.name : "(unnamed)");
                break;
            case WaveX::Protocol::TRACK_BINDING_LOADING:
                snprintf(state,
                         sizeof(state),
                         "Track %u: Instrument %.23s loading",
                         trackDisplayNumber(track),
                         binding.name[0] ? binding.name : "");
                break;
            case WaveX::Protocol::TRACK_BINDING_EMPTY:
            default:
                snprintf(state,
                         sizeof(state),
                         "Track %u: empty - Audition previews; Load + Select enables play",
                         trackDisplayNumber(track));
                break;
        }
    }
    lv_label_set_text_fmt(status_label_,
                          "%s-%s  vel %d  %s  %s",
                          low,
                          high,
                          (int)velocity_,
                          latch_ ? "LATCH" : "",
                          state);
}

// --- parameters ------------------------------------------------------------

void UIPlayPage::onInput(const InputEvent& evt) {
    // The encoder is what makes "hold a note and sweep" physically possible on
    // a single-touch panel. delta is already signed AND the event type names
    // the sign, so take the magnitude and let the type supply direction -
    // reading delta directly is how the sample edit page shipped inverted
    // (roadmap 1.5.2 item 5).
    switch (evt.type) {
        case InputType::EncoderRight:
        case InputType::EncoderUp:
            stepParam(+1);
            break;
        case InputType::EncoderLeft:
        case InputType::EncoderDown:
            stepParam(-1);
            break;
        default:
            break;
    }
}

void UIPlayPage::selectParam(int direction) {
    const int count = static_cast<int>(Param::kCount);
    int idx = static_cast<int>(current_param_) + direction;
    if (idx < 0) {
        idx = count - 1;
    } else if (idx >= count) {
        idx = 0;
    }
    current_param_ = static_cast<Param>(idx);
    refreshParamLabel();
}

void UIPlayPage::stepParam(int direction) {
    const size_t i = static_cast<size_t>(current_param_);
    // Track is a 0..15 Track index, not a continuous CC value - one
    // detent per step, and nothing rides the wire (it only takes effect on
    // the next note-on/off, sent locally from press()/release()).
    if (current_param_ == Param::Track) {
        const uint8_t track = getCurrentTrack();
        int v = static_cast<int>(track) + direction;
        if (v < 0) {
            v = 0;
        } else if (v > 15) {
            v = 15;
        }
        if (static_cast<uint8_t>(v) == track) {
            return;
        }
        setCurrentTrack(static_cast<uint8_t>(v));
        inter_mcu_request_track_binding(currentTrack());
        refreshBindingStatus();
        refreshParamLabel();
        return;
    }
    int v = static_cast<int>(param_value_[i]) + direction * kParamStep;
    if (v < 0) {
        v = 0;
    } else if (v > 65535) {
        v = 65535;
    }
    if (static_cast<uint16_t>(v) == param_value_[i]) {
        return;  // at an end stop; do not spam the link
    }
    param_value_[i] = static_cast<uint16_t>(v);
    sendParam();
    refreshParamLabel();
}

void UIPlayPage::sendParam() {
    const size_t i = static_cast<size_t>(current_param_);
    inter_mcu_send_control_change(kParams[i].wire_param, 0, param_value_[i]);
}

void UIPlayPage::refreshParamLabel() {
    if (!param_label_) {
        return;
    }
    const size_t i = static_cast<size_t>(current_param_);
    // Track is the one Param whose value is not page-local - it lives in the
    // shared current-Track store, so param_value_[Track] is never read.
    const uint16_t raw = current_param_ == Param::Track ? getCurrentTrack() : param_value_[i];
    char value[24];
    FormatParamValue(current_param_, raw, value, sizeof(value));
    lv_label_set_text_fmt(param_label_, "%s  %s", kParams[i].label, value);
}

// --- softkeys --------------------------------------------------------------

std::array<Softkey, NUM_SOFTKEYS> UIPlayPage::getSoftkeys() {
    std::array<Softkey, NUM_SOFTKEYS> keys{};
    keys[0] = {"Back", []() { UINavigator::instance().pop(); }};
    keys[1] = {"< Param", [this]() { selectParam(-1); }};
    keys[2] = {"Param >", [this]() { selectParam(+1); }};
    // Value -/+ duplicate the encoder deliberately: the encoder is the one part
    // of this page that cannot be verified without hardware, and a surface
    // whose only input path might not work is worse than a redundant one.
    keys[3] = {"Value -", [this]() { stepParam(-1); }};
    keys[4] = {"Value +", [this]() { stepParam(+1); }};
    keys[5] = {latch_ ? "Latch*" : "Latch", [this]() {
                   latch_ = !latch_;
                   if (!latch_) {
                       releaseAll();  // leaving latch must not strand held notes
                   }
                   refreshKeys();
                   UINavigator::instance().refreshSoftkeys();
               }};
    return keys;
}

std::array<Softkey, NUM_SOFTKEYS> UIPlayPage::getShiftedSoftkeys() {
    std::array<Softkey, NUM_SOFTKEYS> keys{};
    keys[0] = {"Back", []() { UINavigator::instance().pop(); }};
    keys[1] = {"Oct -", [this]() { setRoot(root_note_ - 12); }};
    keys[2] = {"Oct +", [this]() { setRoot(root_note_ + 12); }};
    keys[3] = {"Semi -", [this]() { setRoot(root_note_ - 1); }};
    keys[4] = {"Semi +", [this]() { setRoot(root_note_ + 1); }};
    // Panic. Cheap insurance while this page is the only note source: a stuck
    // voice with no way to release it is otherwise a reboot.
    keys[5] = {"All Off", [this]() {
                   releaseAll();
                   refreshKeys();
               }};
    return keys;
}

void UIPlayPage::onTrackChanged() {
    // The panel's Track -/+ stepped the shared Track; the binding request is
    // already on the wire, so only the labels need redrawing.
    refreshBindingStatus();
    refreshParamLabel();
}

std::shared_ptr<UIPage> createPlayPage() {
    return std::make_shared<UIPlayPage>();
}

}  // namespace wavex_ui
