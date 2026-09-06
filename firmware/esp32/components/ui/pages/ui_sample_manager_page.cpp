// WaveX Sample Manager page
#include "ui/ui_sample_manager_page.h"

#include <esp_log.h>

#include "../styles/ui_theme.h"
#include "debug/console_command.h"
#include "inter_mcu.h"
#include "ui/current_sample.h"
#include "ui/current_track.h"
#include "ui/ui_navigator.h"
#include "ui/ui_palette.h"

#include <cstdio>
#include <cstring>

namespace wavex_ui {

namespace {
static const char* TAG = "UI_SAMPLE_MGR";

// Local names for the shared palette (ui/ui_palette.h). These were
// hand-copied literals that had already drifted from it and from each
// other - three different "border" greys existed across five files - so a
// theme switch reached only the surfaces that happened to be in sync.
constexpr uint32_t kColPanel = palette::kColCard;
constexpr uint32_t kColBorder = palette::kColBorder;
constexpr uint32_t kColDim = palette::kColDim;
constexpr uint32_t kColGreen = palette::kColGreen;
constexpr uint32_t kColWarn = palette::kColOrange;

// The voice path takes 16-bit mono/stereo only (find_playable_sample on the
// backend). The browser will load 8 and 24-bit files quite happily, so a
// sample can be resident and still unplayable - worth saying on the row rather
// than leaving the user to discover it as silence.
bool meta_is_playable(const WaveX::Protocol::SampleMetadata& m) {
    return m.bits_per_sample == 16 && (m.channels == 1 || m.channels == 2);
}

// Design turn 3a. The Track strip runs the full width; below it the sample
// cards take the left two thirds and the detail pane the right.
constexpr int kStripY = 8;
constexpr int kStripH = 56;
constexpr int kStripGap = 6;
constexpr int kGridY = kStripY + kStripH + 14;
constexpr int kGridW = 820;
constexpr int kGridH = 396;
constexpr int kCardH = 92;
constexpr int kCardGap = 8;
constexpr int kCardW = (kGridW - kCardGap) / 2;
constexpr int kDetailX = UI_MARGIN_X + kGridW + 16;
constexpr int kDetailW = UI_SCREEN_WIDTH - UI_MARGIN_X - kDetailX;

void format_frames(uint32_t frames, uint32_t rate, char* out, size_t n) {
    const uint32_t hz = rate ? rate : 48000;
    const uint32_t total_s = frames / hz;
    snprintf(out, n, "%lu:%02lu", (unsigned long)(total_s / 60), (unsigned long)(total_s % 60));
}
}  // namespace

void UISampleManagerPage::onEnter(lv_obj_t* parent) {
    lv_obj_clean(parent);

    root_ = lv_obj_create(parent);
    lv_obj_remove_style_all(root_);
    lv_obj_set_size(root_, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(root_, UI_COLOR_BG, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(root_, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_remove_flag(root_, LV_OBJ_FLAG_SCROLLABLE);

    buildTrackStrip(root_);

    // The grid holds cards, not rows, so it is a plain container placed
    // absolutely and the cards are placed inside it - a flex column would
    // fight the two-column wrap the design asks for.
    list_ = lv_obj_create(root_);
    lv_obj_remove_style_all(list_);
    lv_obj_set_size(list_, kGridW, kGridH);
    lv_obj_set_pos(list_, UI_MARGIN_X, kGridY);
    lv_obj_set_style_bg_opa(list_, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_remove_flag(list_, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* detail = lv_obj_create(root_);
    lv_obj_remove_style_all(detail);
    lv_obj_set_size(detail, kDetailW, kGridH);
    lv_obj_set_pos(detail, kDetailX, kGridY);
    lv_obj_set_style_bg_color(detail, UI_COLOR_CARD, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(detail, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(detail, UI_RADIUS_CARD, LV_PART_MAIN);
    lv_obj_set_style_border_width(detail, UI_BORDER_WIDTH, LV_PART_MAIN);
    lv_obj_set_style_border_color(detail, UI_COLOR_LINE, LV_PART_MAIN);
    lv_obj_set_style_pad_all(detail, 20, LV_PART_MAIN);
    lv_obj_remove_flag(detail, LV_OBJ_FLAG_SCROLLABLE);

    status_label_ = lv_label_create(detail);
    lv_obj_set_style_text_font(status_label_, UI_FONT_SMALL, LV_PART_MAIN);
    lv_obj_set_style_text_color(status_label_, UI_COLOR_DIM, LV_PART_MAIN);
    lv_obj_set_width(status_label_, kDetailW - 40);
    lv_label_set_long_mode(status_label_, LV_LABEL_LONG_WRAP);
    lv_label_set_text(status_label_, "Samples resident in RAM");
    lv_obj_align(status_label_, LV_ALIGN_TOP_LEFT, 0, 0);

    track_label_ = lv_label_create(detail);
    lv_obj_set_style_text_font(track_label_, UI_FONT_MONO_SMALL, LV_PART_MAIN);
    lv_obj_set_style_text_color(track_label_, UI_COLOR_FG, LV_PART_MAIN);
    lv_obj_align(track_label_, LV_ALIGN_TOP_LEFT, 0, 44);

    detail_label_ = lv_label_create(detail);
    lv_obj_set_style_text_font(detail_label_, UI_FONT_SMALL, LV_PART_MAIN);
    lv_obj_set_style_text_color(detail_label_, UI_COLOR_DIM, LV_PART_MAIN);
    lv_label_set_long_mode(detail_label_, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(detail_label_, kDetailW - 40);
    lv_obj_align(detail_label_, LV_ALIGN_TOP_LEFT, 0, 88);
    lv_label_set_text(detail_label_, "");

    // The list is a window on the Pool, asked for on entry and every refresh
    // tick; single-record pushes (load/edit/unload) keep the detail view
    // current between pages.
    page_first_ = 0;
    binding_probe_ = 0;
    requestPage();
    inter_mcu_request_sample_mem_status();
    inter_mcu_request_track_binding(getCurrentTrack());

    // An lv_timer runs in LVGL context with the lock held, so it may touch
    // widgets directly. Rebuilding from the cache is how new metadata reaches
    // the screen: the comm callback that fills that cache must not draw.
    refresh_timer_ = lv_timer_create(refreshTimerCb, 500, this);

    rebuildList();
    refreshDetail();
    refreshTrackLabel();
    refreshTrackStrip();
}

void UISampleManagerPage::buildTrackStrip(lv_obj_t* parent) {
    strip_ = lv_obj_create(parent);
    lv_obj_remove_style_all(strip_);
    lv_obj_set_size(strip_, UI_SCREEN_WIDTH - 2 * UI_MARGIN_X, kStripH);
    lv_obj_set_pos(strip_, UI_MARGIN_X, kStripY);
    lv_obj_set_style_bg_opa(strip_, LV_OPA_TRANSP, 0);
    lv_obj_remove_flag(strip_, LV_OBJ_FLAG_SCROLLABLE);

    const int total = UI_SCREEN_WIDTH - 2 * UI_MARGIN_X;
    const int cw = (total - (kTrackCount - 1) * kStripGap) / kTrackCount;
    for (int i = 0; i < kTrackCount; ++i) {
        TrackCell& c = track_cells_[i];
        c.cell = lv_obj_create(strip_);
        lv_obj_remove_style_all(c.cell);
        lv_obj_set_size(c.cell, cw, kStripH);
        lv_obj_set_pos(c.cell, i * (cw + kStripGap), 0);
        lv_obj_set_style_bg_color(c.cell, UI_COLOR_CARD, 0);
        lv_obj_set_style_bg_opa(c.cell, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(c.cell, UI_RADIUS_CHIP, 0);
        lv_obj_set_style_border_width(c.cell, UI_BORDER_WIDTH, 0);
        lv_obj_set_style_border_color(c.cell, UI_COLOR_LINE, 0);
        lv_obj_remove_flag(c.cell, LV_OBJ_FLAG_SCROLLABLE);

        c.num = lv_label_create(c.cell);
        lv_obj_set_style_text_font(c.num, UI_FONT_MONO_SMALL, 0);
        lv_obj_set_style_text_color(c.num, UI_COLOR_DIM, 0);
        char n[4];
        snprintf(n, sizeof(n), "%d", i + 1);
        lv_label_set_text(c.num, n);
        lv_obj_align(c.num, LV_ALIGN_CENTER, 0, -7);

        c.bar = lv_obj_create(c.cell);
        lv_obj_remove_style_all(c.bar);
        lv_obj_set_size(c.bar, 24, 4);
        lv_obj_align(c.bar, LV_ALIGN_CENTER, 0, 15);
        lv_obj_set_style_radius(c.bar, 2, 0);
        lv_obj_set_style_bg_opa(c.bar, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(c.bar, UI_COLOR_LINE, 0);
    }
}

// Colour from whatever bindings have arrived so far. A Track we have not
// heard about yet keeps the inert bar rather than being drawn as empty -
// "not known" and "nothing bound" are different, and only one of them is a
// reason to press Assign.
void UISampleManagerPage::refreshTrackStrip() {
    const uint8_t current = getCurrentTrack();
    for (int i = 0; i < kTrackCount; ++i) {
        TrackCell& c = track_cells_[i];
        if (!c.cell || !lv_obj_is_valid(c.cell)) {
            continue;
        }
        WaveX::Protocol::TrackBindingMessage b;
        const bool known = inter_mcu_get_track_binding(static_cast<uint8_t>(i), &b);
        const bool bound = known && b.state == WaveX::Protocol::TRACK_BINDING_SAMPLE;
        const bool sel = (i == current);

        lv_obj_set_style_border_color(c.cell, sel ? UI_COLOR_ACCENT : UI_COLOR_LINE, 0);
        lv_obj_set_style_border_width(c.cell, sel ? UI_BORDER_WIDTH_FOCUS : UI_BORDER_WIDTH, 0);
        lv_obj_set_style_text_color(c.num, sel ? UI_COLOR_FG : UI_COLOR_DIM, 0);
        lv_obj_set_style_bg_color(c.bar, bound ? UI_COLOR_OK : UI_COLOR_LINE, 0);
    }
}

void UISampleManagerPage::onExit() {
    if (refresh_timer_) {
        lv_timer_delete(refresh_timer_);
        refresh_timer_ = nullptr;
    }
    if (root_) {
        lv_obj_del(root_);
        root_ = nullptr;
        list_ = nullptr;
        strip_ = nullptr;
        for (auto& c: track_cells_) {
            c = TrackCell{};
        }
        status_label_ = nullptr;
        detail_label_ = nullptr;
        track_label_ = nullptr;
    }
    for (auto& r: rows_) {
        r = Row{};
    }
    row_count_ = 0;
}

void UISampleManagerPage::refreshTimerCb(lv_timer_t* timer) {
    auto* self = static_cast<UISampleManagerPage*>(lv_timer_get_user_data(timer));
    if (self) {
        inter_mcu_request_track_binding(getCurrentTrack());
        // Four Tracks per tick, round-robin, so the strip fills in about two
        // seconds without putting sixteen requests into a bounded TX queue at
        // once. Re-asking forever also means a binding changed by another page
        // reaches this strip without a page re-entry.
        for (int n = 0; n < 4; ++n) {
            inter_mcu_request_track_binding(self->binding_probe_);
            self->binding_probe_ = static_cast<uint8_t>((self->binding_probe_ + 1) % kTrackCount);
        }
        self->requestPage();
        self->rebuildList();
        self->refreshDetail();
        self->refreshTrackStrip();
    }
}

void UISampleManagerPage::requestPage() {
    inter_mcu_request_sample_meta_page(page_first_, static_cast<uint8_t>(kMaxRows));
}

const UISampleManagerPage::Row* UISampleManagerPage::focusedRow() const {
    if (focus_ < 0 || focus_ >= row_count_) {
        return nullptr;
    }
    return &rows_[focus_];
}

void UISampleManagerPage::rebuildList() {
    if (!list_) {
        return;
    }

    // The last page the Daisy sent for this window. Snapshot it first so the
    // row set cannot change under the rebuild.
    WaveX::Protocol::SampleMetadata metas[kMaxRows];
    uint16_t total = 0;
    uint16_t first = 0;
    int count = static_cast<int>(inter_mcu_get_sample_meta_page(metas, kMaxRows, &total, &first));
    if (first != page_first_) {
        // A page for a window we have since scrolled away from: keep the rows
        // until ours arrives rather than flash an empty list.
        count = -1;
    }
    pool_total_ = total;

    WaveX::Protocol::TrackBindingMessage binding;
    const uint16_t bound_id = inter_mcu_get_track_binding(getCurrentTrack(), &binding) &&
                                      binding.state == WaveX::Protocol::TRACK_BINDING_SAMPLE
                                  ? binding.sample_id
                                  : 0;

    // Runs every call, including the unchanged fast path below: an SFZ
    // import's samples live in the Daisy's own private registry
    // (sfz_loader.cpp), never pushed as MSG_SAMPLE_META, so `count` can sit
    // at 0 indefinitely while an import is actually resident - the "nothing
    // changed" path would otherwise never reach a status update at all. Both
    // registries share one allocator, so RAM in use with zero listed samples
    // is exactly that case, not "nothing loaded" (docs/backlog.md "Sample
    // Manager cannot see an SFZ import's samples" - fixed properly by the
    // shared registry in track-and-patch-model.md §4; this is a status line
    // until then).
    if (count < 0) {
        return;
    }
    if (status_label_) {
        if (total > 0) {
            char s[80];
            if (total > kMaxRows) {
                snprintf(s,
                         sizeof(s),
                         "%u samples resident - showing %u-%u",
                         (unsigned)total,
                         (unsigned)(page_first_ + 1),
                         (unsigned)(page_first_ + count));
            } else {
                snprintf(
                    s, sizeof(s), "%u sample%s resident", (unsigned)total, total == 1 ? "" : "s");
            }
            lv_label_set_text(status_label_, s);
        } else {
            lv_label_set_text(status_label_, "No samples in RAM - load one from Browse");
        }
    }

    // Nothing else changed: leave the widgets alone. Rebuilding a list every
    // 500 ms would restyle rows under the user's finger and throw away focus.
    bool same = (count == row_count_);
    if (same) {
        for (int i = 0; i < count; ++i) {
            if (metas[i].sample_id != rows_[i].sample_id) {
                same = false;
                break;
            }
        }
    }
    if (same) {
        // Selection highlight can still have moved.
        for (int i = 0; i < row_count_; ++i) {
            if (rows_[i].btn && lv_obj_is_valid(rows_[i].btn)) {
                const bool sel = rows_[i].sample_id == bound_id;
                const bool foc = (i == focus_);
                // Focus is the accent border; "bound to the current Track" is
                // the card fill. They are different questions and the card has
                // to be able to answer both at once.
                lv_obj_set_style_border_color(
                    rows_[i].btn, foc ? UI_COLOR_ACCENT : UI_COLOR_LINE, LV_PART_MAIN);
                lv_obj_set_style_border_width(
                    rows_[i].btn, foc ? UI_BORDER_WIDTH_FOCUS : UI_BORDER_WIDTH, LV_PART_MAIN);
                lv_obj_set_style_bg_color(
                    rows_[i].btn, sel ? UI_COLOR_CARD_ALT : UI_COLOR_CARD, LV_PART_MAIN);
            }
        }
        return;
    }

    lv_obj_clean(list_);
    row_count_ = 0;
    for (auto& r: rows_) {
        r = Row{};
    }

    for (int i = 0; i < count; ++i) {
        const auto& m = metas[i];
        const int col = i % 2;
        const int row = i / 2;

        lv_obj_t* btn = lv_obj_create(list_);
        lv_obj_remove_style_all(btn);
        lv_obj_set_size(btn, kCardW, kCardH);
        lv_obj_set_pos(btn, col * (kCardW + kCardGap), row * (kCardH + kCardGap));
        lv_obj_set_style_bg_color(btn, UI_COLOR_CARD, LV_PART_MAIN);
        lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_radius(btn, UI_RADIUS_CARD, LV_PART_MAIN);
        lv_obj_set_style_border_width(btn, UI_BORDER_WIDTH, LV_PART_MAIN);
        lv_obj_set_style_border_color(btn, UI_COLOR_LINE, LV_PART_MAIN);
        lv_obj_set_style_pad_hor(btn, 16, LV_PART_MAIN);
        lv_obj_remove_flag(btn, LV_OBJ_FLAG_SCROLLABLE);

        // The basename, not the path. m.name carries the whole card path, and
        // on a card that fits about twenty characters the leading directories
        // are the part that is identical between every sample - what tells two
        // apart is at the end. The detail pane still shows the full path.
        const char* base = m.name;
        for (const char* p = m.name; *p; ++p) {
            if (*p == '/') {
                base = p + 1;
            }
        }

        lv_obj_t* label = lv_label_create(btn);
        lv_obj_set_style_text_font(label, UI_FONT_TITLE, LV_PART_MAIN);
        lv_obj_set_style_text_color(
            label, meta_is_playable(m) ? UI_COLOR_FG : UI_COLOR_WARN, LV_PART_MAIN);
        // Height pinned to one line as well as width: LONG_DOT only truncates
        // once the text exceeds BOTH, so a height-less label wraps to three
        // lines and spills out of a 92px card before any ellipsis appears.
        lv_obj_set_size(label, kCardW - 32 - 76, 34);
        lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
        lv_label_set_text(label, base);
        lv_obj_align(label, LV_ALIGN_TOP_LEFT, 0, 12);

        // The badge answers "is this one live" - the Track it is bound to, or
        // that it is pinned resident with no user. Those are the two reasons a
        // sample is taking up RAM.
        char badge_text[12] = {};
        bool bound_badge = false;
        for (uint8_t t = 0; t < 16; ++t) {
            if (m.used_by & (1u << t)) {
                snprintf(badge_text, sizeof(badge_text), "T%u", trackDisplayNumber(t));
                bound_badge = true;
                break;
            }
        }
        if (!bound_badge && (m.flags & WaveX::Protocol::SAMPLE_META_PINNED)) {
            snprintf(badge_text, sizeof(badge_text), "loaded");
        }
        lv_obj_t* badge = nullptr;
        if (badge_text[0]) {
            badge = lv_label_create(btn);
            lv_label_set_text(badge, badge_text);
            lv_obj_set_style_text_font(badge, UI_FONT_MICRO, LV_PART_MAIN);
            lv_obj_set_style_radius(badge, UI_RADIUS_BADGE, LV_PART_MAIN);
            lv_obj_set_style_pad_all(badge, 4, LV_PART_MAIN);
            if (bound_badge) {
                lv_obj_set_style_bg_color(badge, UI_COLOR_OK, LV_PART_MAIN);
                lv_obj_set_style_bg_opa(badge, LV_OPA_COVER, LV_PART_MAIN);
                lv_obj_set_style_text_color(badge, UI_COLOR_ACCENT_FG, LV_PART_MAIN);
            } else {
                lv_obj_set_style_border_width(badge, UI_BORDER_WIDTH, LV_PART_MAIN);
                lv_obj_set_style_border_color(badge, UI_COLOR_LINE, LV_PART_MAIN);
                lv_obj_set_style_text_color(badge, UI_COLOR_DIM, LV_PART_MAIN);
            }
            lv_obj_align(badge, LV_ALIGN_TOP_RIGHT, 0, 12);
        }

        // Format chips, mono so the three of them line up card to card.
        const char* chips[3];
        // Wide enough for an unsigned that the compiler cannot bound: these
        // come off the wire, so "24" is expected but not guaranteed.
        char bits[16], rate[16];
        snprintf(bits, sizeof(bits), "%ub", (unsigned)m.bits_per_sample);
        snprintf(rate, sizeof(rate), "%uk", (unsigned)((m.sample_rate + 500) / 1000));
        chips[0] = bits;
        chips[1] = (m.channels == 2) ? "ST" : "MO";
        chips[2] = rate;
        int chip_x = 0;
        for (int c = 0; c < 3; ++c) {
            lv_obj_t* chip = lv_label_create(btn);
            lv_label_set_text(chip, chips[c]);
            lv_obj_set_style_text_font(chip, UI_FONT_MONO_MICRO, LV_PART_MAIN);
            lv_obj_set_style_text_color(chip, UI_COLOR_DIM, LV_PART_MAIN);
            lv_obj_set_style_bg_color(chip, UI_COLOR_CARD_ALT, LV_PART_MAIN);
            lv_obj_set_style_bg_opa(chip, LV_OPA_COVER, LV_PART_MAIN);
            lv_obj_set_style_radius(chip, 4, LV_PART_MAIN);
            lv_obj_set_style_pad_hor(chip, 7, LV_PART_MAIN);
            lv_obj_set_style_pad_ver(chip, 2, LV_PART_MAIN);
            lv_obj_align(chip, LV_ALIGN_BOTTOM_LEFT, chip_x, -12);
            lv_obj_update_layout(chip);
            chip_x += lv_obj_get_width(chip) + 8;
        }

        char dur[16];
        format_frames(m.total_frames, m.sample_rate, dur, sizeof(dur));
        lv_obj_t* dur_label = lv_label_create(btn);
        lv_label_set_text(dur_label, dur);
        lv_obj_set_style_text_font(dur_label, UI_FONT_MONO_SMALL, LV_PART_MAIN);
        lv_obj_set_style_text_color(dur_label, UI_COLOR_FG, LV_PART_MAIN);
        lv_obj_align(dur_label, LV_ALIGN_BOTTOM_RIGHT, 0, -12);

        lv_obj_set_style_bg_color(
            btn, m.sample_id == bound_id ? UI_COLOR_CARD_ALT : UI_COLOR_CARD, LV_PART_MAIN);

        rows_[i].btn = btn;
        rows_[i].label = label;
        rows_[i].badge = badge;
        rows_[i].dur = dur_label;
        rows_[i].sample_id = m.sample_id;
        rows_[i].used_by = m.used_by;
        rows_[i].pinned = (m.flags & WaveX::Protocol::SAMPLE_META_PINNED) != 0;
        rows_[i].playable = meta_is_playable(m);
        ++row_count_;
    }

    if (focus_ >= row_count_) {
        focus_ = row_count_ > 0 ? row_count_ - 1 : 0;
    }
}

void UISampleManagerPage::refreshDetail() {
    if (!detail_label_) {
        return;
    }
    const Row* row = focusedRow();
    if (!row) {
        lv_label_set_text(detail_label_, "");
        return;
    }
    WaveX::Protocol::SampleMetadata m;
    if (!inter_mcu_get_sample_meta(row->sample_id, &m)) {
        lv_label_set_text(detail_label_, "");
        return;
    }

    WaveX::Protocol::TrackBindingMessage binding;
    const bool have_binding = inter_mcu_get_track_binding(getCurrentTrack(), &binding);
    const bool is_bound = have_binding && binding.state == WaveX::Protocol::TRACK_BINDING_SAMPLE &&
                          binding.sample_id == row->sample_id;
    const bool track_holds_patch =
        have_binding && (binding.state == WaveX::Protocol::TRACK_BINDING_PATCH ||
                         binding.state == WaveX::Protocol::TRACK_BINDING_LOADING);
    char action[128];
    if (is_bound) {
        snprintf(action, sizeof(action), "Notes play THIS sample on this Track.");
    } else if (track_holds_patch) {
        snprintf(action,
                 sizeof(action),
                 "Track %u holds Instrument %.24s - Assign replaces it (asks first).",
                 trackDisplayNumber(getCurrentTrack()),
                 binding.name[0] ? binding.name : "(unnamed)");
    } else {
        snprintf(action, sizeof(action), "Press Assign to bind this to the Track.");
    }
    char text[420];
    snprintf(text,
             sizeof(text),
             "%.48s\n\n"
             "id %u   %u Hz   %u-bit %s\n"
             "%lu frames\n"
             "region %lu - %lu\n"
             "loop %s (%lu - %lu)\n"
             "gain %+.1f dB\n\n"
             "%s",
             m.name,
             (unsigned)m.sample_id,
             (unsigned)m.sample_rate,
             (unsigned)m.bits_per_sample,
             m.channels == 2 ? "stereo" : "mono",
             (unsigned long)m.total_frames,
             (unsigned long)m.start_frame,
             (unsigned long)m.end_frame,
             m.loop_enabled ? "on" : "off",
             (unsigned long)m.loop_start,
             (unsigned long)m.loop_end,
             (double)m.gain_db_x10 / 10.0,
             action);
    lv_label_set_text(detail_label_, text);
}

void UISampleManagerPage::refreshTrackLabel() {
    if (!track_label_) {
        return;
    }
    lv_label_set_text_fmt(track_label_, "TRACK %u", trackDisplayNumber(getCurrentTrack()));
}

void UISampleManagerPage::moveFocus(int delta) {
    if (row_count_ == 0) {
        return;
    }
    confirm_assign_id_ = 0;  // the question was about the row that had focus
    // Past either end of the page: turn it, wrapping around the whole Pool
    // the way a single page always wrapped. The rows change when the next
    // page lands; until then the focus sits at the edge.
    const int next = focus_ + delta;
    if (next >= row_count_) {
        const bool more = page_first_ + row_count_ < pool_total_;
        page_first_ = more ? static_cast<uint16_t>(page_first_ + kMaxRows) : 0;
        focus_ = 0;
        requestPage();
    } else if (next < 0) {
        if (page_first_ > 0) {
            page_first_ =
                static_cast<uint16_t>(page_first_ >= kMaxRows ? page_first_ - kMaxRows : 0);
        } else if (pool_total_ > kMaxRows) {
            page_first_ = static_cast<uint16_t>(((pool_total_ - 1) / kMaxRows) * kMaxRows);
        }
        focus_ = kMaxRows - 1;  // clamped to the page's row count once it lands
        requestPage();
    } else {
        focus_ = next;
    }
    rebuildList();
    refreshDetail();
}

void UISampleManagerPage::changeTrack(int delta) {
    // 16 Tracks - matches instrument.hpp's kNumTracks and
    // MSG_NOTE_ON's channel & 0x0F on the backend.
    setCurrentTrack(static_cast<uint8_t>((getCurrentTrack() + delta + 16) % 16));
    inter_mcu_request_track_binding(getCurrentTrack());
    onTrackChanged();
}

void UISampleManagerPage::onTrackChanged() {
    confirm_assign_id_ = 0;  // the confirm was about the Track that was selected
    refreshTrackLabel();
    rebuildList();
    refreshDetail();
}

void UISampleManagerPage::assignFocused() {
    const Row* row = focusedRow();
    if (!row) {
        return;
    }
    if (!row->playable) {
        // Selecting it would be accepted and then silently ignored at note-on,
        // which is the failure mode this page exists to make visible.
        if (status_label_) {
            lv_label_set_text(status_label_,
                              "Not playable: the voice path takes 16-bit mono/stereo only");
        }
        return;
    }
    WaveX::Protocol::TrackBindingMessage current;
    const bool known = inter_mcu_get_track_binding(getCurrentTrack(), &current);
    if (known && current.state == WaveX::Protocol::TRACK_BINDING_LOADING) {
        if (status_label_) {
            lv_label_set_text(status_label_,
                              "Track is still loading an Instrument - wait, or pick another");
        }
        return;
    }
    // Replacing is always confirmed (§6.2): a Track that holds a different
    // sample, or an Instrument, asks once. With the Sample Pool an import's
    // samples are released per Track, so replacing an Instrument with a
    // sample is an ordinary bind - one the user cannot undo, hence the
    // question. Re-binding the same sample is a no-op and needs none; an
    // empty Track needs none either.
    const bool holds_instrument = known && current.state == WaveX::Protocol::TRACK_BINDING_PATCH;
    const bool occupied =
        holds_instrument || (known && current.state == WaveX::Protocol::TRACK_BINDING_SAMPLE &&
                             current.sample_id != row->sample_id);
    if (occupied && confirm_assign_id_ != row->sample_id) {
        confirm_assign_id_ = row->sample_id;
        if (status_label_) {
            char what[48];
            if (holds_instrument) {
                snprintf(what,
                         sizeof(what),
                         "Instrument %.24s",
                         current.name[0] ? current.name : "(unnamed)");
            } else {
                WaveX::Protocol::SampleMetadata m;
                const bool named = inter_mcu_get_sample_meta(current.sample_id, &m) && m.name[0];
                snprintf(what, sizeof(what), "%.40s", named ? m.name : "a sample");
            }
            char msg[160];
            snprintf(msg,
                     sizeof(msg),
                     "Track %u holds %s - press Assign again to replace it",
                     trackDisplayNumber(getCurrentTrack()),
                     what);
            lv_label_set_text(status_label_, msg);
        }
        return;
    }
    confirm_assign_id_ = 0;
    if (inter_mcu_send_sample_select(row->sample_id, getCurrentTrack()) == ESP_OK) {
        inter_mcu_request_track_binding(getCurrentTrack());
        ESP_LOGI(TAG,
                 "Requested sample %u for Track %u",
                 (unsigned)row->sample_id,
                 trackDisplayNumber(getCurrentTrack()));
    } else if (status_label_) {
        lv_label_set_text(status_label_, "Assign failed - link busy?");
    }
    rebuildList();
    refreshDetail();
}

void UISampleManagerPage::unloadFocused() {
    const Row* row = focusedRow();
    if (!row) {
        return;
    }
    const uint16_t id = row->sample_id;
    if (inter_mcu_send_sample_unload(id) != ESP_OK) {
        if (status_label_) {
            lv_label_set_text(status_label_, "Unload failed - link busy?");
        }
        return;
    }
    // The row disappears when the backend's metadata push lands, not here: the
    // backend is the authority on what is resident, and guessing would let the
    // list drift from RAM the moment an unload failed on that side.
    if (status_label_) {
        lv_label_set_text(status_label_, "Unloading...");
    }
}

// Sets the row's sample as Sample Edit's current sample (track-and-patch-model.md
// §6 stage 0). Does not switch tabs - the Edit tab already sits alongside this
// page in the Sample group; this just decides what it will show when opened.
void UISampleManagerPage::editFocused() {
    const Row* row = focusedRow();
    if (!row) {
        return;
    }
    setCurrentSampleId(row->sample_id);
    if (status_label_) {
        char s[64];
        snprintf(s,
                 sizeof(s),
                 "Sample %u set for editing - open the Edit tab",
                 (unsigned)row->sample_id);
        lv_label_set_text(status_label_, s);
    }
}

void UISampleManagerPage::onInput(const InputEvent& evt) {
    switch (evt.type) {
        // Clockwise (positive steps()) moves focus forward. EncoderDown used
        // to mean forward here - a compensation for a backwards-counting
        // encoder. Direction is set once per encoder in hardware_config.h
        // (WAVEX_*_DIRECTION), never in a page.
        case InputType::EncoderRight:
        case InputType::EncoderLeft:
        case InputType::EncoderUp:
        case InputType::EncoderDown:
            moveFocus(evt.steps() > 0 ? +1 : -1);
            break;
        case InputType::EncoderClick:
            assignFocused();
            break;
        default:
            break;
    }
}

std::array<Softkey, NUM_SOFTKEYS> UISampleManagerPage::getSoftkeys() {
    std::array<Softkey, NUM_SOFTKEYS> keys{};
    keys[0] = {"Back", []() { UINavigator::instance().pop(); }};
    keys[1] = {"Assign", [this]() { assignFocused(); }};
    keys[2] = {"Unload", [this]() { unloadFocused(); }};
    keys[3] = {"Up", [this]() { moveFocus(-1); }};
    keys[4] = {"Down", [this]() { moveFocus(+1); }};
    keys[5] = {"Refresh", [this]() {
                   requestPage();
                   inter_mcu_request_sample_mem_status();
                   inter_mcu_request_track_binding(getCurrentTrack());
               }};
    return keys;
}

std::array<Softkey, NUM_SOFTKEYS> UISampleManagerPage::getShiftedSoftkeys() {
    std::array<Softkey, NUM_SOFTKEYS> keys{};
    keys[0] = {"Back", []() { UINavigator::instance().pop(); }};
    keys[1] = {"Track -", [this]() { changeTrack(-1); }};
    keys[2] = {"Track +", [this]() { changeTrack(+1); }};
    keys[3] = {"Edit", [this]() { editFocused(); }};
    return keys;
}

// Debug harness: the focused row and the status line, which is where Assign
// explains itself (refused / confirm / done).
size_t UISampleManagerPage::consoleState(char* out, size_t cap, size_t len) {
    using namespace WaveX::Debug;
    const Row* row = focusedRow();
    len = AppendKvInt(out, cap, len, "rows", row_count_);
    len = AppendKvInt(out, cap, len, "total", pool_total_);
    len = AppendKvInt(out, cap, len, "first", page_first_);
    len = AppendKvInt(out, cap, len, "focusid", row ? row->sample_id : 0);
    len = AppendKvInt(out, cap, len, "focusused", row ? row->used_by : 0);
    len = AppendKvInt(out, cap, len, "focuspinned", row && row->pinned ? 1 : 0);
    len = AppendKvText(
        out, cap, len, "status", status_label_ ? lv_label_get_text(status_label_) : "");
    return len;
}

std::shared_ptr<UIPage> createSampleManagerPage() {
    return std::make_shared<UISampleManagerPage>();
}

}  // namespace wavex_ui
