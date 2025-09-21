/**
 * @file file_browser.h
 * @brief File Browser Component for SD Card File Operations
 * 
 * This component provides file browsing functionality for the SD card,
 * including directory listing, file filtering, and metadata display.
 */

#ifndef WAVEX_FILE_BROWSER_H
#define WAVEX_FILE_BROWSER_H

#include "lvgl.h"
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// File entry structure
typedef struct {
    bool is_directory;
    uint32_t size_bytes;
    char name[48];
    char path[96];
} wavex_file_entry_t;

// File browser configuration
typedef struct {
    const char* root_path;           // Root directory path
    const char* file_extension;      // File extension filter (e.g., ".wav")
    uint32_t max_entries;            // Maximum number of entries to display
    bool show_hidden;                // Show hidden files
} wavex_file_browser_config_t;

// File browser callbacks
typedef void (*wavex_file_selected_cb_t)(const wavex_file_entry_t* entry, void* user_data);
typedef void (*wavex_directory_changed_cb_t)(const char* path, void* user_data);

// File browser structure
typedef struct {
    lv_obj_t* container;             // Main container
    lv_obj_t* list;                  // List object for file entries
    lv_obj_t* path_label;            // Current path display
    wavex_file_entry_t* entries;     // File entries array
    uint32_t entry_count;            // Number of entries
    uint32_t selected_index;         // Currently selected entry
    char current_path[96];           // Current directory path
    wavex_file_browser_config_t config;
    wavex_file_selected_cb_t file_selected_cb;
    wavex_directory_changed_cb_t dir_changed_cb;
    void* user_data;
} wavex_file_browser_t;

// File browser functions
wavex_file_browser_t* wavex_file_browser_create(lv_obj_t* parent, const wavex_file_browser_config_t* config);
void wavex_file_browser_destroy(wavex_file_browser_t* browser);

// Navigation functions
bool wavex_file_browser_navigate_to(wavex_file_browser_t* browser, const char* path);
bool wavex_file_browser_navigate_up(wavex_file_browser_t* browser);
bool wavex_file_browser_refresh(wavex_file_browser_t* browser);

// Selection functions
void wavex_file_browser_set_selection(wavex_file_browser_t* browser, uint32_t index);
const wavex_file_entry_t* wavex_file_browser_get_selected(wavex_file_browser_t* browser);
uint32_t wavex_file_browser_get_selected_index(wavex_file_browser_t* browser);

// Callback functions
void wavex_file_browser_set_file_selected_callback(wavex_file_browser_t* browser, 
                                                   wavex_file_selected_cb_t callback, 
                                                   void* user_data);
void wavex_file_browser_set_directory_changed_callback(wavex_file_browser_t* browser, 
                                                       wavex_directory_changed_cb_t callback, 
                                                       void* user_data);

// Utility functions
const char* wavex_file_browser_get_current_path(wavex_file_browser_t* browser);
uint32_t wavex_file_browser_get_entry_count(wavex_file_browser_t* browser);
const wavex_file_entry_t* wavex_file_browser_get_entry(wavex_file_browser_t* browser, uint32_t index);

#ifdef __cplusplus
}
#endif

#endif // WAVEX_FILE_BROWSER_H
