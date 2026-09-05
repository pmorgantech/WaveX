// WaveX Sample Manager page
#include "ui/ui_sample_manager_page.h"

#include <esp_log.h>

#include "../styles/ui_theme.h"
#include "inter_mcu.h"
#include "ui/current_sample.h"
#include "ui/current_track.h"
#include "ui/ui_navigator.h"

#include <cstdio>
#include <cstring>

namespace wavex_ui {

namespace {
static const char* TAG = "UI_SAMPLE_MGR";

constexpr uint32_t kColPanel = 0x0E0E0E;
constexpr uint32_t kColBorder = 0x222222;
constexpr uint32_t kColDim = 0x8FA0AA;
constexpr uint32_t kColGreen = 0x4CAF50;
constexpr uint32_t kColWarn = 0xFF9800;

// The voice path takes 16-bit mono/stereo only (find_playable_sample on the
// backend). The browser will load 8 and 24-bit files quite happily, so a
// sample can be resident and still unplayable - worth saying on the row rather
// than leaving the user to discover it as silence.
bool meta_is_playable(const WaveX::Protocol::SampleMetadata& m) {
    return m.bits_per_sample == 16 && (m.channels == 1 || m.channels == 2);
}

void format_frames(uint32_t frames, uint32_t rate, char* out, size_t n) {
    const uint32_t hz = rate ? rate : 48000;
    const uint32_t total_s = frames / hz;
    snprintf(out, n, "%lu:%02lu", (unsigned long)(total_s / 60), (unsigned long)(total_s % 60));
}
}  // namespace

void UISampleManagerPage::onEnter(lv_obj_t* parent) {
    lv_obj_clean(parent);

    root_ = lv_obj_create(parent);
    lv_obj_set_size(root_, lv_pct(100), lv_pct(100));
    ui_theme_apply_container_style(root_, true);
    lv_obj_set_style_pad_all(root_, UI_PADDING_MEDIUM, LV_PART_MAIN);
    lv_obj_remove_flag(root_, LV_OBJ_FLAG_SCROLLABLE);

    status_label_ = lv_label_create(root_);
    ui_theme_apply_label_style(status_label_, false);
    lv_obj_set_style_text_color(status_label_, lv_color_hex(kColDim), LV_PART_MAIN);
    lv_label_set_text(status_label_, "Samples resident in RAM");
    lv_obj_set_pos(status_label_, 0, 0);

    track_label_ = lv_label_create(root_);
    ui_theme_apply_label_style(track_label_, false);
    lv_obj_set_pos(track_label_, 794, 0);

    list_ = lv_obj_create(root_);
    lv_obj_set_size(list_, 770, 430);
    lv_obj_set_pos(list_, 0, 34);
    lv_obj_set_style_bg_color(list_, lv_color_hex(kColPanel), LV_PART_MAIN);
    lv_obj_set_style_border_width(list_, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(list_, lv_color_hex(kColBorder), LV_PART_MAIN);
    lv_obj_set_style_pad_all(list_, 6, LV_PART_MAIN);
    lv_obj_set_flex_flow(list_, LV_FLEX_FLOW_COLUMN);

    detail_label_ = lv_label_create(root_);
    ui_theme_apply_label_style(detail_label_, false);
    lv_obj_set_style_text_color(detail_label_, lv_color_hex(kColDim), LV_PART_MAIN);
    lv_label_set_long_mode(detail_label_, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(detail_label_, 470);
    lv_obj_set_pos(detail_label_, 794, 34);
    lv_label_set_text(detail_label_, "");

    // The backend pushes metadata on load/edit/unload, but a page opened later
    // has missed those, so ask for the current set on entry.
    inter_mcu_request_sample_meta(0);
    inter_mcu_request_sample_mem_status();
    inter_mcu_request_track_binding(getCurrentTrack());

    // An lv_timer runs in LVGL context with the lock held, so it may touch
    // widgets directly. Rebuilding from the cache is how new metadata reaches
    // the screen: the comm callback that fills that cache must not draw.
    refresh_timer_ = lv_timer_create(refreshTimerCb, 500, this);

    rebuildList();
    refreshDetail();
    refreshTrackLabel();
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
        self->rebuildList();
        self->refreshDetail();
    }
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

    // Snapshot the cache first so the row set cannot change under the rebuild.
    WaveX::Protocol::SampleMetadata metas[kMaxRows];
    int count = 0;
    for (uint16_t id = 1; id <= 64 && count < kMaxRows; ++id) {
        WaveX::Protocol::SampleMetadata m;
        if (inter_mcu_get_sample_meta(id, &m)) {
            metas[count++] = m;
        }
    }

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
    if (status_label_) {
        if (count > 0) {
            char s[64];
            snprintf(s, sizeof(s), "%d sample%s resident", count, count == 1 ? "" : "s");
            lv_label_set_text(status_label_, s);
        } else {
            WaveX::Protocol::SampleMemStatusMessage mem{};
            inter_mcu_get_sample_mem_status(&mem);
            lv_label_set_text(status_label_,
                              mem.in_use_bytes > 0
                                  ? "An SFZ import is resident - its samples aren't listable yet"
                                  : "No samples in RAM - load one from Browse");
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
                lv_obj_set_style_border_color(
                    rows_[i].btn, lv_color_hex(foc ? kColGreen : kColBorder), LV_PART_MAIN);
                lv_obj_set_style_border_width(rows_[i].btn, foc ? 2 : 1, LV_PART_MAIN);
                lv_obj_set_style_bg_color(
                    rows_[i].btn, lv_color_hex(sel ? 0x14261A : kColPanel), LV_PART_MAIN);
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
        lv_obj_t* btn = lv_obj_create(list_);
        lv_obj_set_size(btn, lv_pct(100), 48);
        lv_obj_set_style_radius(btn, 4, LV_PART_MAIN);
        lv_obj_set_style_pad_left(btn, 12, LV_PART_MAIN);
        lv_obj_remove_flag(btn, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t* label = lv_label_create(btn);
        ui_theme_apply_label_style(label, false);
        lv_obj_align(label, LV_ALIGN_LEFT_MID, 0, 0);

        char dur[16];
        format_frames(m.total_frames, m.sample_rate, dur, sizeof(dur));
        char line[160];
        snprintf(line,
                 sizeof(line),
                 "%u  %.40s   %u-bit %s  %s%s",
                 (unsigned)m.sample_id,
                 m.name,
                 (unsigned)m.bits_per_sample,
                 m.channels == 2 ? "stereo" : "mono",
                 dur,
                 meta_is_playable(m) ? "" : "   [not playable]");
        lv_label_set_text(label, line);
        lv_obj_set_style_text_color(
            label, lv_color_hex(meta_is_playable(m) ? 0xFFFFFF : kColWarn), LV_PART_MAIN);
        lv_obj_set_style_bg_color(
            btn, lv_color_hex(m.sample_id == bound_id ? 0x14261A : kColPanel), LV_PART_MAIN);

        rows_[i].btn = btn;
        rows_[i].label = label;
        rows_[i].sample_id = m.sample_id;
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
                 "Track %u holds Instrument %.24s - Assign is refused here.",
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
    focus_ = (focus_ + delta + row_count_) % row_count_;
    confirm_assign_id_ = 0;  // the question was about the row that had focus
    rebuildList();
    refreshDetail();
}

void UISampleManagerPage::changeTrack(int delta) {
    // 16 Tracks - matches instrument.hpp's kNumTracks and
    // MSG_NOTE_ON's channel & 0x0F on the backend.
    setCurrentTrack(static_cast<uint8_t>((getCurrentTrack() + delta + 16) % 16));
    confirm_assign_id_ = 0;  // and about the Track that was selected
    inter_mcu_request_track_binding(getCurrentTrack());
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
    // A Track holding an SFZ Instrument refuses a bare-sample bind on the backend
    // (SfzLoader::BindSample): the import owns its samples and can only
    // release them through the load handshake. That refusal used to reach
    // nothing but the Daisy log, so Assign looked broken rather than
    // declined - say which Track and why, here, before sending.
    WaveX::Protocol::TrackBindingMessage current;
    const bool known = inter_mcu_get_track_binding(getCurrentTrack(), &current);
    if (known && (current.state == WaveX::Protocol::TRACK_BINDING_PATCH ||
                  current.state == WaveX::Protocol::TRACK_BINDING_LOADING)) {
        if (status_label_) {
            char msg[128];
            snprintf(msg,
                     sizeof(msg),
                     "Track %u holds Instrument %.24s - Assign refused; pick another Track",
                     trackDisplayNumber(getCurrentTrack()),
                     current.name[0] ? current.name : "(unnamed)");
            lv_label_set_text(status_label_, msg);
        }
        return;
    }
    // Replacing is always confirmed (§6.2): a Track that holds a different
    // sample asks once. Re-binding the same sample is a no-op and needs no
    // question; an empty Track needs none either.
    const bool occupied = known && current.state == WaveX::Protocol::TRACK_BINDING_SAMPLE &&
                          current.sample_id != row->sample_id;
    if (occupied && confirm_assign_id_ != row->sample_id) {
        confirm_assign_id_ = row->sample_id;
        if (status_label_) {
            WaveX::Protocol::SampleMetadata m;
            const bool named = inter_mcu_get_sample_meta(current.sample_id, &m) && m.name[0];
            char msg[160];
            snprintf(msg,
                     sizeof(msg),
                     "Track %u holds %.32s - press Assign again to replace it",
                     trackDisplayNumber(getCurrentTrack()),
                     named ? m.name : "a sample");
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
        case InputType::EncoderRight:
        case InputType::EncoderDown:
            moveFocus(+1);
            break;
        case InputType::EncoderLeft:
        case InputType::EncoderUp:
            moveFocus(-1);
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
                   inter_mcu_request_sample_meta(0);
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

std::shared_ptr<UIPage> createSampleManagerPage() {
    return std::make_shared<UISampleManagerPage>();
}

}  // namespace wavex_ui
