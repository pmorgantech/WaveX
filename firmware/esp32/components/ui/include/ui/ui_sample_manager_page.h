// WaveX Sample Manager: what is resident in sample RAM, and what to do with it
#pragma once

#include "input_event.h"
#include "spi_protocol/protocol.h"
#include "ui_page.h"

#include <array>
#include <cstdint>
#include <memory>

namespace wavex_ui {

/**
 * @brief Lists the samples resident in the backend's sample RAM and acts on them.
 *
 * The Sample Browser answers "what is on the card"; this answers "what is in
 * RAM, which one do notes play, and how do I get it back". Those were not
 * separable before: the backend had no notion of a selected sample, so notes
 * always addressed whichever was loaded last, and nothing could free one.
 *
 * Rows come from the frontend's SampleMetadata cache, which the backend pushes
 * on every load, edit and unload - the frontend never derives them. A sample is
 * flagged unplayable here rather than silently failing at note-on: the voice
 * path takes 16-bit mono/stereo only, while the browser will happily load 8 and
 * 24-bit files.
 *
 * "Assign" binds the focused sample to a Track (0..15, matches
 * MSG_NOTE_ON's channel and the Play page's own Track parameter) rather than
 * to "notes on any channel" - that any-channel behavior was retired (roadmap
 * Phase 2.5 item 1) because it made a Track's note-on resolve to whatever was
 * most recently loaded ANYWHERE, not something this page's own binding
 * controlled. Shift+Track -/+ changes which Track Assign targets. A Track
 * that already holds a sample asks before it is replaced (§1.3 / §6.2 of
 * track-and-patch-model.md): the first Assign names what it would replace,
 * the second, on the same row, does it.
 */
class UISampleManagerPage : public UIPage {
   public:
    explicit UISampleManagerPage(bool select_only = false) : select_only_(select_only) {}
    const char* name() const override { return select_only_ ? "Select sample" : "Sample Manager"; }

    void onEnter(lv_obj_t* parent) override;
    size_t consoleState(char* out, size_t cap, size_t len) override;
    void onExit() override;
    void onInput(const InputEvent& evt) override;
    std::array<Softkey, NUM_SOFTKEYS> getSoftkeys() override;
    std::array<Softkey, NUM_SOFTKEYS> getShiftedSoftkeys() override;

   private:
    const bool select_only_;
    void selectFocused();
    // One page of the Sample Pool: the window this page asks the Daisy for
    // (MSG_SAMPLE_META_PAGE_REQ) and shows. The Pool holds up to 1024; the
    // list pages through it rather than mirroring it.
    static constexpr int kMaxRows = 8;

    /// One sample card (design turn 3a). The widgets are kept because a
    /// focus or binding change is then a style write on two cards rather than
    /// a rebuild of the grid - the same reason the root menu keeps its rows.
    struct Row {
        lv_obj_t* btn = nullptr;
        lv_obj_t* label = nullptr;  ///< sample name
        lv_obj_t* badge = nullptr;  ///< "T3" when bound, "loaded" when pinned
        lv_obj_t* dur = nullptr;
        uint16_t sample_id = 0;
        uint16_t used_by = 0;
        bool playable = false;
        bool pinned = false;
    };

    /// One cell of the 16-Track strip across the top: the Track number and a
    /// bar saying whether anything is bound to it. This is the only place the
    /// whole Track set is visible at once, which is what makes "which Track am
    /// I about to overwrite" answerable before pressing Assign rather than
    /// after.
    static constexpr int kTrackCount = 16;
    struct TrackCell {
        lv_obj_t* cell = nullptr;
        lv_obj_t* num = nullptr;
        lv_obj_t* bar = nullptr;
    };

    lv_obj_t* root_ = nullptr;
    lv_obj_t* list_ = nullptr;
    lv_obj_t* status_label_ = nullptr;
    lv_obj_t* detail_label_ = nullptr;
    lv_obj_t* track_label_ = nullptr;
    lv_obj_t* strip_ = nullptr;
    TrackCell track_cells_[kTrackCount] = {};
    /// The tick only asks and only redraws when these have moved: a Pool
    /// change re-requests the window, an arrival rebuilds from the caches.
    uint32_t pool_revision_seen_ = 0;
    uint32_t cache_revision_seen_ = 0;
    /// The last page request did not get onto the link; the tick retries it.
    bool page_request_pending_ = false;
    static constexpr uint32_t kRefreshMs = 250;
    lv_timer_t* refresh_timer_ = nullptr;

    Row rows_[kMaxRows] = {};
    int row_count_ = 0;
    int focus_ = 0;
    /// Index (among resident records) of the first row shown; moving the
    /// focus past either end turns the page.
    uint16_t page_first_ = 0;
    uint16_t pool_total_ = 0;
    void requestPage();

    static void refreshTimerCb(lv_timer_t* timer);
    void rebuildList();                 ///< UI task / LVGL context only.
    void styleRows(uint16_t bound_id);  ///< focus ring and bound fill on every card
    void refreshDetail();               ///< UI task / LVGL context only.
    void refreshTrackLabel();
    void buildTrackStrip(lv_obj_t* parent);
    void refreshTrackStrip();
    void assignFocused();
    /// Sample id the last Assign press asked to confirm for, 0 = none. A
    /// second press on the same row within the same Track replaces.
    uint16_t confirm_assign_id_ = 0;
    void unloadFocused();
    void editFocused();
    void moveFocus(int delta);
    void changeTrack(int delta);
    void onTrackChanged() override;
    const Row* focusedRow() const;
};

std::shared_ptr<UIPage> createSampleManagerPage();
std::shared_ptr<UIPage> createSamplePickerPage();

}  // namespace wavex_ui
