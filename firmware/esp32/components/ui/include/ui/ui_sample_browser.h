// WaveX UI Sample Browser Page
#pragma once

#include <lvgl.h>

#include "../components/file_browser.h"
#include "comm/i_comm_interface.h"
#include "input_event.h"
#include "inter_mcu.h"
#include "ui_navigator.h"
#include "ui_page.h"

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
    }

    // Check if state is valid
    bool isValid() const { return !current_directory_path.empty() && selected_file_index >= 0; }

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

    // State
    bool is_playing_ = false;
    bool is_initialized_ = false;
    std::string current_directory_;  // Track current directory to detect actual changes  // Track
                                     // if browser is fully initialized
    uint32_t selected_file_index_ = 0;
    char selected_file_path_[96] = {0};

    // Deferred UI update flags (for thread-safe updates from callbacks)
    bool status_update_pending_ = false;
    bool metadata_update_pending_ = false;
    char pending_status_text_[256] = {0};
    char pending_metadata_text_[512] = {0};
    const wavex_file_entry_t* pending_metadata_entry_ = nullptr;

    // State is now managed by the caller (UI navigator) to avoid static globals

    // Callback handlers
    static void file_selected_callback(const wavex_file_entry_t* entry, void* user_data);
    static void file_selected_index_callback(uint32_t file_index,
                                             const wavex_file_entry_t* entry,
                                             void* user_data);
    static void directory_changed_callback(const char* path, void* user_data);

    // Sample status callback handler (called from inter-MCU system)
    static void sample_status_callback(uint8_t state,
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

}  // namespace wavex_ui
