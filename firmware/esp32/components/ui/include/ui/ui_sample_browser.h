// WaveX UI Sample Browser Page
#pragma once

#include "ui_page.h"
#include "ui_navigator.h"
#include "input_event.h"
#include <lvgl.h>
#include <memory>
#include <cstring>
#include "../components/file_browser.h"
#include "../../../main/inter_mcu.h"

namespace wavex_ui {

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
    UISampleBrowser();
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
    
    // State
    bool is_playing_ = false;
    uint32_t selected_file_index_ = 0;
    char selected_file_path_[96] = {0};
    
    // Deferred UI update flags (for thread-safe updates from callbacks)
    bool status_update_pending_ = false;
    bool metadata_update_pending_ = false;
    char pending_status_text_[256] = {0};
    char pending_metadata_text_[512] = {0};
    const wavex_file_entry_t* pending_metadata_entry_ = nullptr;
    
    // Preserve state across destroy/create cycles
    static char s_last_directory_path_[];
    static uint32_t s_last_selected_index_;
    
    // Callback handlers
    static void file_selected_callback(const wavex_file_entry_t* entry, void* user_data);
    static void file_selected_index_callback(uint32_t file_index, const wavex_file_entry_t* entry, void* user_data);
    static void directory_changed_callback(const char* path, void* user_data);
    
    // Sample status callback handler (called from inter-MCU system)
    static void sample_status_callback(uint8_t state, uint32_t sample_rate, uint8_t channels, uint32_t frames_played, void* user_data);

private:
    // Internal methods
    void updateStatus(const char* status);
    void updateMetadata(const wavex_file_entry_t* entry);
    void processDeferredUpdates_(); // Internal implementation
    bool auditionSampleByIndex(uint32_t file_index);
    bool stopAudition();
    void refreshSoftkeys();
    
    // Global instance for callbacks (temporary, until we have better callback architecture)
    static UISampleBrowser* s_active_instance_;
};

/**
 * @brief Factory function to create a sample browser page
 */
std::shared_ptr<UIPage> createSampleBrowserPage();

} // namespace wavex_ui
