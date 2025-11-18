/**
 * @file file_browser.h
 * @brief File Browser Component for SD Card File Operations
 *
 * This component provides file browsing functionality for the SD card,
 * including directory listing, file filtering, and metadata display.
 */

#ifndef WAVEX_FILE_BROWSER_H
#define WAVEX_FILE_BROWSER_H

#include <stdbool.h>
#include <stdint.h>

#include "lvgl.h"

/* Keep headers C-friendly without forcing C linkage on downstream C++ headers */

// File entry structure
typedef struct {
    bool is_directory;
    uint32_t size_bytes;
    char name[48];
    char path[96];
    // WAV metadata (only valid for audio files)
    uint32_t sample_rate;      // 0 if not a WAV file or unknown
    uint16_t channels;         // 0 if not a WAV file or unknown
    uint16_t bits_per_sample;  // 0 if not a WAV file or unknown
    uint32_t duration_ms;      // Duration in milliseconds (0 if unknown)
} wavex_file_entry_t;

// Forward declaration for comm interface
namespace WaveX {
namespace Comm {
class ICommInterface;
}
}  // namespace WaveX

// File browser configuration
typedef struct {
    const char* root_path;                        // Root directory path
    const char* file_extension;                   // File extension filter (e.g., ".wav")
    uint32_t max_entries;                         // Maximum number of entries to display
    bool show_hidden;                             // Show hidden files
    WaveX::Comm::ICommInterface* comm_interface;  // Communication interface
} wavex_file_browser_config_t;

// File browser callbacks
typedef void (*wavex_file_selected_cb_t)(const wavex_file_entry_t* entry, void* user_data);
typedef void (*wavex_file_selected_index_cb_t)(uint32_t file_index,
                                               const wavex_file_entry_t* entry,
                                               void* user_data);
typedef void (*wavex_directory_changed_cb_t)(const char* path, void* user_data);

// File browser structure
typedef struct {
    lv_obj_t* container;          // Main container
    lv_obj_t* list;               // List object for file entries
    lv_obj_t* path_label;         // Current path display
    wavex_file_entry_t* entries;  // File entries array
    uint32_t entry_count;         // Number of entries
    uint32_t selected_index;      // Currently selected entry
    char current_path[96];        // Current directory path
    wavex_file_browser_config_t config;
    wavex_file_selected_cb_t file_selected_cb;
    wavex_file_selected_index_cb_t file_selected_index_cb;
    wavex_directory_changed_cb_t dir_changed_cb;
    void* user_data;

    // Pagination state
    uint32_t total_files;         // Total number of files in directory
    uint32_t current_page;        // Current page being displayed
    uint32_t entries_per_page;    // Entries per page (typically 4)
    bool pagination_in_progress;  // True if we're currently loading more pages
    uint32_t loaded_entries;      // Number of entries loaded so far

    // UI update flags (for thread-safe deferred updates)
    bool ui_update_pending;  // True if UI needs to be updated

    // Scrolling/viewport state
    uint32_t first_visible_index;  // Index of first visible entry (for scrolling)
    uint32_t visible_count;        // Number of entries visible on screen
} wavex_file_browser_t;

// File browser functions
wavex_file_browser_t* wavex_file_browser_create(lv_obj_t* parent,
                                                const wavex_file_browser_config_t* config);
void wavex_file_browser_destroy(wavex_file_browser_t* browser);

// Navigation functions
bool wavex_file_browser_navigate_to(wavex_file_browser_t* browser, const char* path);
bool wavex_file_browser_navigate_up(wavex_file_browser_t* browser);
bool wavex_file_browser_refresh(wavex_file_browser_t* browser);

// Selection functions
void wavex_file_browser_set_selection(wavex_file_browser_t* browser, uint32_t index);
const wavex_file_entry_t* wavex_file_browser_get_selected(wavex_file_browser_t* browser);
uint32_t wavex_file_browser_get_selected_index(wavex_file_browser_t* browser);

// Navigation functions with boundary checking and scrolling
bool wavex_file_browser_navigate_up_entry(
    wavex_file_browser_t* browser);  // Move selection up (respects boundaries)
bool wavex_file_browser_navigate_down_entry(
    wavex_file_browser_t* browser);  // Move selection down (respects boundaries)

// Callback functions
void wavex_file_browser_set_file_selected_callback(wavex_file_browser_t* browser,
                                                   wavex_file_selected_cb_t callback,
                                                   void* user_data);
void wavex_file_browser_set_file_selected_index_callback(wavex_file_browser_t* browser,
                                                         wavex_file_selected_index_cb_t callback,
                                                         void* user_data);
void wavex_file_browser_set_directory_changed_callback(wavex_file_browser_t* browser,
                                                       wavex_directory_changed_cb_t callback,
                                                       void* user_data);

// Thread-safe UI update
void wavex_file_browser_process_pending_updates(wavex_file_browser_t* browser);

// Utility functions
const char* wavex_file_browser_get_current_path(wavex_file_browser_t* browser);
uint32_t wavex_file_browser_get_entry_count(wavex_file_browser_t* browser);
const wavex_file_entry_t* wavex_file_browser_get_entry(wavex_file_browser_t* browser,
                                                       uint32_t index);

#endif  // WAVEX_FILE_BROWSER_H
