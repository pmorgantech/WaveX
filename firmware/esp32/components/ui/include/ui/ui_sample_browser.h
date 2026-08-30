// WaveX UI Sample Browser Page
#pragma once

#include <lvgl.h>

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

namespace wavex_ui {

/**
 * @brief Persistent state for the Sample Browser
 *
 * This structure maintains the browser's state across page navigation,
 * allowing seamless restoration of the user's browsing context.
 */
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

    // Default constructor
    SampleBrowserState() = default;

    // Copy constructor and assignment
    SampleBrowserState(const SampleBrowserState&) = default;
    SampleBrowserState& operator=(const SampleBrowserState&) = default;

    // Reset to default state
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

    // Check if state is valid
    // Note: no index check - selected_file_index is unsigned and 0 (the
    // first entry) is a valid selection, so a directory is all it takes.
    bool isValid() const { return !current_directory_path.empty(); }

    // Update directory and reset selection
    void changeDirectory(const std::string& new_path) {
        current_directory_path = new_path;
        selected_file_index = 0;
        // Note: We keep is_playing state when changing directories
    }

    // Update selected file
    void selectFile(uint32_t index, const std::string& path = "") {
        selected_file_index = index;
        if (!path.empty()) {
            playing_sample_path = path;
            playing_sample_index = index;
        }
    }

    // Start playback
    void startPlayback(uint32_t index, const std::string& path = "") {
        is_playing = true;
        playing_sample_index = index;
        if (!path.empty()) {
            playing_sample_path = path;
        }
    }

    // Stop playback
    void stopPlayback() {
        is_playing = false;
        playing_sample_path.clear();
        playing_sample_index = 0;
    }

    // Reserve a new sample ID (skips 0 which is used as sentinel)
    uint16_t allocateSampleId() {
        uint16_t id = next_sample_id++;
        if (next_sample_id == 0) {
            next_sample_id = 1;
        }
        return id;
    }
};

/**
 * @brief Sample Browser page for browsing and auditioning audio samples
 *
 * This page provides file browsing functionality with the ability to:
 * - Browse directories and files on SD card
 * - View file metadata
 * - Audition samples (play/stop)
 * - Navigate with encoder and buttons
 */
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

    // Static method for UI task to process updates (public API)
    static void processDeferredUpdates();

   private:
    // UI components
    lv_obj_t* browser_container_ = nullptr;
    lv_obj_t* info_panel_ = nullptr;
    lv_obj_t* status_label_ = nullptr;
    lv_obj_t* metadata_label_ = nullptr;

    // File browser component (C-style, but we wrap it)
    wavex_file_browser_t* file_browser_ = nullptr;

    // Communication interface
    WaveX::Comm::ICommInterface* comm_interface_ = nullptr;

    // Persistent state (owned by caller, injected via constructor)
    SampleBrowserState& persistent_state_;

    // Detail panel (design 1b).
    lv_obj_t* listing_label_ = nullptr;  // "1-20 of 63 - name ^"
    lv_obj_t* card_label_ = nullptr;     // "SD 12.4 GB free" / card state
    lv_obj_t* detail_name_ = nullptr;
    lv_obj_t* play_bar_ = nullptr;

    void refreshStatusStrip();

    // State
    bool is_playing_ = false;
    bool is_initialized_ = false;
    std::string current_directory_;  // Track current directory to detect actual changes  // Track
                                     // if browser is fully initialized
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

    // State is now managed by the caller (UI navigator) to avoid static globals

    // Callback handlers
    static void file_selected_callback(const wavex_file_entry_t* entry, void* user_data);
    static void file_selected_index_callback(uint32_t file_index,
                                             const wavex_file_entry_t* entry,
                                             void* user_data);
    static void directory_changed_callback(const char* path, void* user_data);

    // Sample status callback handler (called from inter-MCU system)
    static void sample_status_callback(uint16_t sample_id,
                                       uint8_t state,
                                       uint32_t sample_rate,
                                       uint8_t channels,
                                       uint32_t frames_played,
                                       void* user_data);

   private:
    // Internal methods
    void updateStatus(const char* status);
    void updateMetadata(const wavex_file_entry_t* entry);
    void processDeferredUpdates_();  // Internal implementation
    bool auditionSampleByIndex(uint32_t file_index);
    bool stopAudition();
    void refreshSoftkeys();
    bool loadSample(const wavex_file_entry_t* entry);

    // Global instance for callbacks (temporary, until we have better callback architecture)
    static UISampleBrowser* s_active_instance_;
};

/**
 * @brief Factory function to create a sample browser page
 *
 * This function maintains persistent state across page instances using
 * dependency injection rather than static variables in the class.
 */
std::shared_ptr<UIPage> createSampleBrowserPage(WaveX::Comm::ICommInterface& comm_interface);

/**
 * @brief Access the shared SampleBrowserState used by the browser page.
 *
 * Returns nullptr if the browser has not been created yet.
 */
SampleBrowserState* getSampleBrowserState();

}  // namespace wavex_ui
