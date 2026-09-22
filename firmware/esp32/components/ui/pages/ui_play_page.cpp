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
#include <cstring>

namespace wavex_ui {

using namespace wavex_ui::palette;

namespace {

constexpr int kMidiNoteMax = 127;

// Content geometry. The panel is 720x1280 rotated to 1280x720; the navigator
// takes UI_HEADER_HEIGHT (75) off the top and UI_HOTKEY_HEIGHT (100) off the
// bottom, and the tab bar takes 56 more. Positions are absolute against that,
// matching how the diagnostics cards are laid out rather than introducing a
// second convention.
constexpr int kDesignW = UI_SCREEN_WIDTH;
constexpr int kContentH = UI_CONTENT_HEIGHT - kEncoderStripHeight;
// The status/parameter strip sits ABOVE the tabview, not inside a tab. It has
// to: the parameters are page-scoped, and a strip built into one tab body would
// vanish when the other tab was selected - taking the only readout of what the
// encoder is editing with it.
// The in-page status strip is gone: it said which Track was bound and what
// the focused parameter was, both of which the header now carries. The tab
// body therefore starts directly under the tab bar.
constexpr int kTabBodyH = kContentH - UI_TAB_BAR_HEIGHT;  // 501

// Design turn 3b: pads on the left, parameter column on the right.
constexpr int kPadGridW = 900;
constexpr int kColX = UI_MARGIN_X + kPadGridW + 16;
constexpr int kColW = UI_SCREEN_WIDTH - UI_MARGIN_X - kColX;
constexpr int kColGap = 10;
constexpr int kBigTileH = 150;
constexpr int kSmallTileH = 100;
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
// Not themed on purpose: a piano keyboard reads as a piano keyboard because
// its keys are white and black. Tinting these per theme would cost the
// instant recognition the surface depends on.
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
};

// Control identities; values come from correlated Instrument readback.
const ParamSpec kParams[static_cast<size_t>(UIPlayPage::Param::kCount)] = {
    {"CUTOFF", WaveX::Protocol::PARAM_FILTER_CUTOFF},
    {"RES", WaveX::Protocol::PARAM_FILTER_RESONANCE},
    {"ATTACK", WaveX::Protocol::PARAM_ENVELOPE_ATTACK},
    {"DECAY", WaveX::Protocol::PARAM_ENVELOPE_DECAY},
    {"SUSTAIN", WaveX::Protocol::PARAM_ENVELOPE_SUSTAIN},
    {"RELEASE", WaveX::Protocol::PARAM_ENVELOPE_RELEASE},
    // wire_param is unused for Track - stepParam()/sendParam() special-case it
    // rather than sending MSG_CONTROL_CHANGE, since it addresses which
    // Track a note-on goes out on, not a voice parameter value.
    {"TRACK", 0},
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
    controls_.Reset(currentTrack());
    controls_alive_ = inter_mcu_backend_link_alive();

    root_ = lv_obj_create(parent);
    lv_obj_set_size(root_, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(root_, lv_color_hex(kColBg), LV_PART_MAIN);
    lv_obj_set_style_border_width(root_, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(root_, 0, LV_PART_MAIN);
    lv_obj_remove_flag(root_, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* tab_host = lv_obj_create(root_);
    lv_obj_set_size(tab_host, lv_pct(100), kContentH);
    encoder_strip_.Create(root_, UI_CONTENT_HEIGHT - kEncoderStripHeight);
    lv_obj_set_pos(tab_host, 0, 0);
    lv_obj_set_style_bg_color(tab_host, lv_color_hex(kColBg), LV_PART_MAIN);
    lv_obj_set_style_border_width(tab_host, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(tab_host, 0, LV_PART_MAIN);
    lv_obj_remove_flag(tab_host, LV_OBJ_FLAG_SCROLLABLE);

    tabview_ = tabGroupCreate(tab_host);
    lv_obj_t* t_pads = tabGroupAddTab(tabview_, "Pads");
    keys_tab_ = tabGroupAddTab(tabview_, "Keys");
    keys_built_ = false;

    buildPads(t_pads);
    buildParamColumn(t_pads);
    // Build the keyboard on first use; Pads entry needs only its 16 keys.

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
    binding_requested_at_ = lv_tick_get();
    serviceParameters();
    binding_timer_ = lv_timer_create(bindingTimerCb, 20, this);
}

void UIPlayPage::onExit() {
    encoder_strip_.Reset();
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
        tabview_ = keys_tab_ = nullptr;
        keys_built_ = false;

        param_tile_ = ValueTile{};
        octave_tile_ = ValueTile{};
        semi_tile_ = ValueTile{};
        velocity_tile_ = ValueTile{};
        latch_tile_ = ValueTile{};
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
        if (!self->keys_built_ && lv_tabview_get_tab_active(self->tabview_) == 1) {
            self->buildKeys(self->keys_tab_);
            self->keys_built_ = true;
        }
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
    self->serviceParameters();
    if (lv_tick_get() - self->binding_requested_at_ >= 500) {
        self->binding_requested_at_ = lv_tick_get();
        inter_mcu_request_track_binding(self->currentTrack());
        self->refreshBindingStatus();
    }
    self->refreshParamLabel();
    UINavigator::instance().refreshSoftkeys();
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
    // style list, which allocates. This page can build 41 keys, each a button
    // plus a label, so the six local properties these used to set were ~500
    // property stores on a single page entry - and page entry is the whole
    // cost of this page (docs/roadmap.md). What genuinely varies per key is
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
        lv_style_set_text_font(&s_label_base, UI_FONT_SMALL);
        lv_style_set_text_align(&s_label_base, LV_TEXT_ALIGN_CENTER);

        s_styles_ready = true;
    }

    lv_obj_t* btn = lv_btn_create(parent);
    // Drop the theme's default button styling before adding ours. The default
    // theme applies a substantial style to every button it sees - gradients,
    // shadows, transitions, pressed transforms - across up to 41 keys. None
    // of it survives our styling visually, so discard it before adding ours.
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
    k.drawn_bg = bg;
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
void UIPlayPage::buildParamColumn(lv_obj_t* parent) {
    // One full tile for the parameter the encoder is on, then four small ones
    // for the performance state you change between phrases. Octave and semi
    // are derived from the root note rather than stored twice; latch and Track
    // are read from where they already live.
    param_tile_ = valueTileCreate(parent, kColX, 0, kColW, kBigTileH, "PARAM", nullptr);
    // The tile edits whichever parameter it is currently showing, through the
    // same stepParam() the Value -/+ keys and the encoder use.
    valueTileSetOnAdjust(param_tile_, [this](int steps) {
        const int dir = steps > 0 ? 1 : -1;
        for (int n = 0; n < (steps > 0 ? steps : -steps); ++n) {
            stepParam(dir);
        }
        refreshPadTiles();
    });

    const int half = (kColW - kColGap) / 2;
    const int row1 = kBigTileH + kColGap;
    const int row2 = row1 + kSmallTileH + kColGap;
    octave_tile_ = valueTileCreate(parent, kColX, row1, half, kSmallTileH, "OCTAVE", nullptr);
    semi_tile_ =
        valueTileCreate(parent, kColX + half + kColGap, row1, half, kSmallTileH, "SEMI", nullptr);
    velocity_tile_ = valueTileCreate(parent, kColX, row2, half, kSmallTileH, "VELOCITY", nullptr);
    latch_tile_ =
        valueTileCreate(parent, kColX + half + kColGap, row2, half, kSmallTileH, "LATCH", nullptr);

    // None of the four small tiles is a quantity in a range, so none of them
    // gets a fill bar - an empty track would read as "zero".
    valueTileHideFill(octave_tile_);
    valueTileHideFill(semi_tile_);
    valueTileHideFill(velocity_tile_);
    valueTileHideFill(latch_tile_);
}

void UIPlayPage::buildPads(lv_obj_t* tab) {
    const int gap = 8;
    // A margin top and bottom so the last row does not sit flush against the
    // softkey cards - two rows of touch targets with no gap between them is
    // how you hit the wrong one.
    const int pad_y = 8;
    const int cell_w = (kPadGridW - gap * (kPadCols - 1)) / kPadCols;
    const int cell_h = (kKeysH - 2 * pad_y - gap * (kPadRows - 1)) / kPadRows;

    for (int r = 0; r < kPadRows; ++r) {
        for (int c = 0; c < kPadCols; ++c) {
            const int8_t offset = static_cast<int8_t>(r * kPadCols + c);
            lv_obj_t* btn = makeKey(tab, offset, false, kColCard, 0xFFFFFF);
            if (!btn) {
                continue;
            }
            lv_obj_set_size(btn, cell_w, cell_h);
            lv_obj_set_pos(
                btn, UI_MARGIN_X + c * (cell_w + gap), kKeysY + pad_y + r * (cell_h + gap));
            lv_obj_align(keys_[key_count_ - 1].label, LV_ALIGN_CENTER, 0, 0);
            // A pad is 220x119 and its note name is the whole content, read
            // at arm's length while playing. The heading step is sized for a
            // row title competing with other text; nothing competes here.
            lv_obj_set_style_text_font(keys_[key_count_ - 1].label, UI_FONT_DISPLAY, LV_PART_MAIN);
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
    k.sent_track = currentTrack();
    k.down = inter_mcu_send_note_on_track(k.sent_note, velocity_, k.sent_track) == ESP_OK;
}

void UIPlayPage::release(int index) {
    Key& k = keys_[index];
    if (!k.down) {
        return;  // no matching press (e.g. RELEASED after PRESS_LOST)
    }
    if (inter_mcu_send_note_off_track(k.sent_note, k.sent_track) == ESP_OK)
        k.down = false;
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
        const int note = noteFor(k);
        if (k.drawn_note != note) {
            NoteName(note, name, sizeof(name));
            lv_label_set_text(k.label, name);
            k.drawn_note = static_cast<int16_t>(note);
        }
        // Latched keys stay lit: a sustaining note you cannot see is one you
        // forget about. Resting colour comes from the key itself rather than
        // from working out which surface it belongs to.
        const uint32_t bg = k.down ? kColGreen : k.bg_normal;
        if (k.drawn_bg != bg) {
            lv_obj_set_style_bg_color(k.obj, lv_color_hex(bg), LV_PART_MAIN);
            k.drawn_bg = bg;
        }
    }

    refreshPadTiles();
    refreshBindingStatus();
}

void UIPlayPage::refreshBindingStatus() {
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
    // The header carries the binding - what a note-on will actually do. The
    // note range, velocity and latch are performance state and live in the
    // tiles beside the pads, where they can be read without moving your eyes
    // off the surface you are playing.
    snprintf(context_line_, sizeof(context_line_), "%s-%s / %s", low, high, state);
    UINavigator::instance().refreshContext();
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

void UIPlayPage::stepParam(int direction, int divisor) {
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
        onTrackChanged();
        return;
    }
    if (!parameterReady())
        return;
    int v = static_cast<int>(controls_.Value(static_cast<unsigned>(i))) +
            direction * kParamStep / divisor;
    if (v < 0) {
        v = 0;
    } else if (v > 65535) {
        v = 65535;
    }
    if (static_cast<uint16_t>(v) == param_value_[i]) {
        return;  // at an end stop; do not spam the link
    }
    if (inter_mcu_send_control_change(
            kParams[i].wire_param, currentTrack(), static_cast<uint16_t>(v)) != ESP_OK)
        return;
    controls_.Edited(static_cast<unsigned>(i));
    serviceParameters();
    refreshParamLabel();
}

bool UIPlayPage::parameterReady() const {
    return current_param_ == Param::Track ||
           (inter_mcu_backend_link_alive() && controls_.Track() == currentTrack() &&
            controls_.Ready(static_cast<unsigned>(current_param_), lv_tick_get()));
}

void UIPlayPage::serviceParameters() {
    const bool alive = inter_mcu_backend_link_alive();
    if (alive != controls_alive_ || controls_.Track() != currentTrack()) {
        controls_.Reset(currentTrack());
        controls_alive_ = alive;
    }
    if (!alive)
        return;
    const auto now = lv_tick_get();
    WaveX::Protocol::InstEditSyncMessage filter;
    WaveX::Protocol::InstModSyncMessage envelope;
    if (inter_mcu_get_instrument_edit(&filter))
        controls_.Accept(filter, now);
    if (inter_mcu_get_modulator(&envelope))
        controls_.Accept(envelope, now);
    for (unsigned i = 0; i < 6; ++i)
        param_value_[i] = controls_.Value(i);
    // Page-specific request namespace, monotonic across exit/re-entry.
    static uint32_t next_id = 0x504C0000;
    for (unsigned group = 0; group < 2; ++group) {
        if (!controls_.Due(group, now))
            continue;
        if (++next_id == 0)
            ++next_id;
        esp_err_t result;
        if (group == 0) {
            WaveX::Protocol::InstEditOpMessage request;
            request.track = currentTrack();
            request.request_id = next_id;
            result = inter_mcu_send_instrument_edit(request);
        } else {
            WaveX::Protocol::InstModOpMessage request;
            request.track = currentTrack();
            request.request_id = next_id;
            result = inter_mcu_send_modulator(request);
        }
        // Pace failed admissions too; a later read is safe to retry.
        controls_.Requested(group, result == ESP_OK ? next_id : 0, now);
    }
}

void UIPlayPage::refreshParamLabel() {
    refreshPadTiles();
}

// Everything in the right-hand column, from state that already exists
// elsewhere - nothing here is a second copy the page has to keep in step.
void UIPlayPage::refreshPadTiles() {
    if (!param_tile_.card || !lv_obj_is_valid(param_tile_.card)) {
        return;
    }
    const size_t i = static_cast<size_t>(current_param_);
    // Track is the one Param whose value is not page-local - it lives in the
    // shared current-Track store, so param_value_[Track] is never read.
    const uint16_t raw = current_param_ == Param::Track ? getCurrentTrack() : param_value_[i];
    char value[24];
    if (parameterReady())
        FormatParamValue(current_param_, raw, value, sizeof(value));
    else
        snprintf(value, sizeof(value), "--");

    if (std::strcmp(lv_label_get_text(param_tile_.label), kParams[i].label))
        lv_label_set_text(param_tile_.label, kParams[i].label);
    valueTileSetValue(param_tile_, value);
    valueTileSetFocus(param_tile_, true);
    if (current_param_ == Param::Track || !parameterReady()) {
        valueTileHideFill(param_tile_);
    } else {
        valueTileSetFill(param_tile_, static_cast<float>(raw) / 65535.0f);
    }

    char buf[16];
    // Octave and semitone are two readings of the one root note, not two
    // stored values - MIDI note 60 is octave 5, semitone 0.
    snprintf(buf, sizeof(buf), "%d", root_note_ / 12);
    valueTileSetValue(octave_tile_, buf);
    snprintf(buf, sizeof(buf), "%d", root_note_ % 12);
    valueTileSetValue(semi_tile_, buf);

    snprintf(buf, sizeof(buf), "%d", (int)velocity_);
    valueTileSetValue(velocity_tile_, buf);

    // Latch is a mode, so the tile is lit while it is on rather than just
    // spelling the word - it changes what the next pad press means.
    valueTileSetValue(latch_tile_, latch_ ? "on" : "off");
    valueTileSetFocus(latch_tile_, latch_);
}

// --- softkeys --------------------------------------------------------------

EncoderBindings UIPlayPage::encoderBindings() {
    EncoderBindings bindings;
    for (uint8_t i = 0; i < 4; ++i) {
        auto& binding = bindings[i];
        binding.label = kParams[i].label;
        FormatParamValue(
            static_cast<Param>(i), param_value_[i], binding.value.data(), binding.value.size());
        binding.owner = this;
        binding.parameter = i;
        binding.enabled = inter_mcu_backend_link_alive() && controls_.Track() == currentTrack() &&
                          controls_.Ready(i, lv_tick_get());
        if (!binding.enabled)
            snprintf(binding.value.data(), binding.value.size(), "--");
        binding.onSteps = [](void* owner, uint8_t parameter, int steps) {
            auto& page = *static_cast<UIPlayPage*>(owner);
            page.current_param_ = static_cast<Param>(parameter);
            page.stepParam(steps, 4);
        };
    }
    return bindings;
}

PanelPageLeds UIPlayPage::panelLeds() const {
    PanelPageLeds result;
    for (int i = 0; i < kPadCount && i < key_count_; ++i) {
        if (keys_[i].obj)
            result.defined |= static_cast<uint16_t>(1u << i);
        if (keys_[i].down)
            result.active |= static_cast<uint16_t>(1u << i);
    }
    return result;
}

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
    for (int i: {3, 4}) {
        keys[i].enabled = parameterReady();
        keys[i].why = "Waiting for sound values, or value outside Play range";
    }
    keys[5].active = latch_;
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
    controls_.Reset(currentTrack());
    serviceParameters();
    // The panel's Track -/+ stepped the shared Track; the binding request is
    // already on the wire, so only the labels need redrawing.
    refreshBindingStatus();
    refreshParamLabel();
}

std::shared_ptr<UIPage> createPlayPage() {
    return std::make_shared<UIPlayPage>();
}

}  // namespace wavex_ui
