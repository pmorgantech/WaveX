// WaveX UI Sample Browser Page
#pragma once

#include <lvgl.h>

#include "../components/envelope_fetcher.h"
#include "../components/file_browser.h"
#include "comm/i_comm_interface.h"
#include "input_event.h"
#include "inter_mcu.h"
#include "ui_navigator.h"
#include "ui_page.h"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace wavex_ui {

/// Browser state kept alive across page navigation (see createSampleBrowserPage),
/// so leaving and returning to the browser restores the same directory/selection.
struct SampleBrowserState {
    std::string current_directory_path = "/";
    uint32_t selected_file_index = 0;
    bool is_playing = false;
    std::string playing_sample_path = "";
    uint32_t playing_sample_index = 0;
    uint16_t next_sample_id = 1;
    uint16_t last_load_sample_id = 0;
    std::string last_load_sample_path = "";

    // Geometry of the loaded sample, captured from the browse listing. The
    // edit page previously assumed a fixed 48000 frames, which is exactly one
    // second at 48 kHz - hence markers that could not move past 1 s and a zoom
    // that would not open out. Nothing else tells the frontend how long a
    // sample is: MSG_SAMPLE_STATUS reports frames *played*, not total.
    uint32_t last_load_sample_rate = 0;  // Hz, 0 if unknown
    uint32_t last_load_duration_ms = 0;  // 0 if unknown
    uint16_t last_load_channels = 0;
    uint16_t last_load_bits = 0;
    uint32_t last_load_size_bytes = 0;

    /** Total frames, or 0 if the geometry is unknown. */
    uint32_t lastLoadFrames() const {
        if (last_load_sample_rate == 0 || last_load_duration_ms == 0) {
            return 0;
        }
        return static_cast<uint32_t>(
            (static_cast<uint64_t>(last_load_duration_ms) * last_load_sample_rate) / 1000ull);
    }

    SampleBrowserState() = default;
    SampleBrowserState(const SampleBrowserState&) = default;
    SampleBrowserState& operator=(const SampleBrowserState&) = default;

    void reset() {
        current_directory_path = "/";
        selected_file_index = 0;
        is_playing = false;
        playing_sample_path.clear();
        playing_sample_index = 0;
        last_load_sample_id = 0;
        last_load_sample_path.clear();
        last_load_sample_rate = 0;
        last_load_duration_ms = 0;
        last_load_channels = 0;
        last_load_bits = 0;
        last_load_size_bytes = 0;
    }

    // No index check: selected_file_index is unsigned and 0 (the first entry)
    // is a valid selection, so a directory is all it takes.
    bool isValid() const { return !current_directory_path.empty(); }

    void changeDirectory(const std::string& new_path) {
        current_directory_path = new_path;
        selected_file_index = 0;
        // is_playing is deliberately kept across a directory change.
    }

    void selectFile(uint32_t index, const std::string& path = "") {
        selected_file_index = index;
        if (!path.empty()) {
            playing_sample_path = path;
            playing_sample_index = index;
        }
    }

    void startPlayback(uint32_t index, const std::string& path = "") {
        is_playing = true;
        playing_sample_index = index;
        if (!path.empty()) {
            playing_sample_path = path;
        }
    }

    void stopPlayback() {
        is_playing = false;
        playing_sample_path.clear();
        playing_sample_index = 0;
    }

    // Skips 0, which is reserved as a sentinel.
    uint16_t allocateSampleId() {
        uint16_t id = next_sample_id++;
        if (next_sample_id == 0) {
            next_sample_id = 1;
        }
        return id;
    }
};

/// Browses and auditions SD-card samples: directory/file listing, metadata
/// display, play/stop audition, and encoder/button navigation.
class UISampleBrowser : public UIPage {
   public:
    explicit UISampleBrowser(WaveX::Comm::ICommInterface& comm_interface,
                             SampleBrowserState& persistent_state);
    ~UISampleBrowser() override;

    const char* name() const override { return "Sample Browser"; }

    void onEnter(lv_obj_t* parent) override;
    void onExit() override;
    void onInput(const InputEvent& evt) override;
    std::array<Softkey, NUM_SOFTKEYS> getSoftkeys() override;

    // UI task calls this to drain the deferred updates queued by the RX task.
    static void processDeferredUpdates();

   private:
    lv_obj_t* browser_container_ = nullptr;
    lv_obj_t* info_panel_ = nullptr;
    lv_obj_t* status_label_ = nullptr;
    lv_obj_t* metadata_label_ = nullptr;

    // File browser component (C-style, but we wrap it)
    wavex_file_browser_t* file_browser_ = nullptr;

    WaveX::Comm::ICommInterface* comm_interface_ = nullptr;

    // Persistent state (owned by caller, injected via constructor)
    SampleBrowserState& persistent_state_;

    // Detail panel (design 1b).
    lv_obj_t* listing_label_ = nullptr;  // "1-20 of 63 - name ^"
    lv_obj_t* card_label_ = nullptr;     // "SD 12.4 GB free" / card state
    lv_obj_t* detail_name_ = nullptr;
    lv_obj_t* play_bar_ = nullptr;

    // Waveform preview of the LOADED sample (roadmap 1.5.3).
    //
    // Only the loaded sample can be shown: MSG_ENVELOPE_REQ is served from
    // sample RAM on the Daisy, so there is nothing to draw for a file that has
    // only been selected. That constraint is also what makes this affordable -
    // the roadmap's worry was that a per-selection round trip would make
    // scrolling unusable, and this asks for nothing while the cursor moves.
    std::unique_ptr<class WaveformView> waveform_;
    lv_obj_t* waveform_hint_ = nullptr;
    EnvelopeFetcher envelope_fetcher_;
    std::vector<WaveX::Protocol::EnvelopeColumn> envelope_columns_;

    /// The sample the panel is currently drawing. serviceWaveform() runs on
    /// every deferred-update pass (~30 Hz), so it needs to recognise its own
    /// steady state: without these it would re-render 442 columns and
    /// invalidate the widget on every pass for a waveform that has not moved.
    uint16_t shown_sample_id_ = 0;
    uint16_t shown_generation_ = 0;
    bool waveform_drawn_ = false;
    /// Stops re-asking forever once the fetcher has exhausted its retries.
    /// Cleared when the sample changes, so a different selection tries again.
    bool waveform_gave_up_ = false;

    static void envelopeChunkStatic(const WaveX::Protocol::EnvelopeChunkMessage& header,
                                    const WaveX::Protocol::EnvelopeColumn* columns,
                                    void* user);
    void serviceWaveform();
    void drawWaveform();
    /// True when the highlighted row is the sample that is actually loaded.
    bool selectionIsLoadedSample() const;

    void refreshStatusStrip();

    bool is_playing_ = false;
    bool is_initialized_ = false;    // true once the file browser is fully set up
    std::string current_directory_;  // used to detect actual directory changes
    uint32_t selected_file_index_ = 0;
    char selected_file_path_[96] = {0};

    // Deferred UI updates. The producer is the UART RX task (sample-status and
    // browse callbacks); the consumer is the UI task, which drains these under
    // the LVGL lock in processDeferredUpdates_(). Widgets must never be touched
    // from the producer side - the LVGL task renders on the other core.
    //
    // The flags are release-stored after their payload is written and
    // acquire-loaded before it is read, so the consumer cannot see a raised
    // flag ahead of the data it advertises. Plain bools (or volatile) give no
    // such ordering on this dual-core part.
    std::atomic<bool> status_update_pending_{false};
    std::atomic<bool> metadata_update_pending_{false};
    char pending_status_text_[256] = {0};
    char pending_metadata_text_[512] = {0};
    const wavex_file_entry_t* pending_metadata_entry_ = nullptr;

    // Playback bar position, published as a percentage by the status callback.
    std::atomic<bool> play_bar_update_pending_{false};
    std::atomic<int> pending_play_bar_pct_{0};

    // Softkey row needs rebuilding (play/stop labels follow is_playing_).
    std::atomic<bool> softkey_refresh_pending_{false};

    // Selection metadata should be re-read from the file browser once a
    // directory listing has landed.
    std::atomic<bool> selection_metadata_pending_{false};

    // SFZ inspection/load replies arrive on the UART task and are consumed by
    // the UI task. Separate request ids keep selection probes and a live load
    // from invalidating one another.
    std::atomic<uint32_t> probe_request_id_{0};
    std::atomic<uint32_t> load_request_id_{0};
    uint32_t next_instrument_request_id_ = 1;
    bool sfz_selected_ = false;
    bool sfz_probe_ready_ = false;
    bool sfz_probe_loadable_ = false;
    uint8_t sfz_sample_count_ = 0;
    char sfz_probe_path_[96] = {};
    WaveX::Protocol::InstStatusMessage pending_inst_status_{};
    portMUX_TYPE inst_status_lock_ = portMUX_INITIALIZER_UNLOCKED;
    std::atomic<bool> inst_status_update_pending_{false};

    static void file_selected_callback(const wavex_file_entry_t* entry, void* user_data);
    static void file_selected_index_callback(uint32_t file_index,
                                             const wavex_file_entry_t* entry,
                                             void* user_data);
    static void directory_changed_callback(const char* path, void* user_data);

    // Called from the inter-MCU RX path.
    static void sample_status_callback(uint16_t sample_id,
                                       uint8_t state,
                                       uint32_t sample_rate,
                                       uint8_t channels,
                                       uint32_t frames_played,
                                       void* user_data);
    static void instrument_status_callback(const WaveX::Protocol::InstStatusMessage& status,
                                           void* user_data);

   private:
    void updateStatus(const char* status);
    void updateMetadata(const wavex_file_entry_t* entry);
    void processDeferredUpdates_();
    bool auditionSampleByIndex(uint32_t file_index);
    bool stopAudition();
    void refreshSoftkeys();
    bool loadSample(const wavex_file_entry_t* entry);
    bool loadInstrument(const wavex_file_entry_t* entry);
    void requestInstrumentProbe(const wavex_file_entry_t* entry);

    // Global instance for callbacks (temporary, until we have better callback architecture)
    static UISampleBrowser* s_active_instance_;
};

// Persistent state across page instances is injected rather than kept in
// static variables - see SampleBrowserState.
std::shared_ptr<UIPage> createSampleBrowserPage(WaveX::Comm::ICommInterface& comm_interface);

/// Returns nullptr if the browser has not been created yet.
SampleBrowserState* getSampleBrowserState();

}  // namespace wavex_ui
