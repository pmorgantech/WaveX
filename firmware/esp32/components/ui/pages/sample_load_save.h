/**
 * @file sample_load_save.h
 * @brief Sample Load/Save Page Implementation
 *
 * This page provides file browsing and sample audition functionality
 * for loading and saving audio samples from the SD card.
 */

#ifndef WAVEX_SAMPLE_LOAD_SAVE_H
#define WAVEX_SAMPLE_LOAD_SAVE_H

#include "../components/file_browser.h"
#include "lvgl.h"

// Forward declaration for comm interface
namespace WaveX {
namespace Comm {
class ICommInterface;
}
}  // namespace WaveX

/* Public C API: these declarations are C-friendly. Avoid forcing C linkage on
 * transitively included C++ headers by keeping extern "C" minimal and only in
 * implementation files where needed. */

// Sample load/save page structure
typedef struct {
    lv_obj_t* main_container;
    lv_obj_t* file_browser_container;
    wavex_file_browser_t* file_browser;
    lv_obj_t* info_panel;
    lv_obj_t* status_label;
    lv_obj_t* metadata_label;
    bool is_playing;
    char selected_file[96];
    uint32_t selected_file_index;
    WaveX::Comm::ICommInterface* comm_interface;  // Communication interface

    // Deferred UI update flags (for thread-safe updates from callbacks)
    bool status_update_pending;
    bool metadata_update_pending;
    char pending_status_text[256];
    char pending_metadata_text[512];
    const wavex_file_entry_t*
        pending_metadata_entry;  // Pointer to entry data (valid until next update)
} wavex_sample_load_save_page_t;

// Page creation and management
wavex_sample_load_save_page_t* wavex_sample_load_save_create(
    lv_obj_t* parent, WaveX::Comm::ICommInterface* comm_interface);
void wavex_sample_load_save_destroy(wavex_sample_load_save_page_t* page);
wavex_sample_load_save_page_t* wavex_sample_load_save_get_active(void);

// Page interface functions
void wavex_sample_load_save_show(wavex_sample_load_save_page_t* page);
void wavex_sample_load_save_hide(wavex_sample_load_save_page_t* page);
void wavex_sample_load_save_update(wavex_sample_load_save_page_t* page);

// Sample operations
bool wavex_sample_load_save_audition_sample(wavex_sample_load_save_page_t* page,
                                            const char* file_path);
bool wavex_sample_load_save_audition_sample_by_index(wavex_sample_load_save_page_t* page,
                                                     uint32_t file_index);
bool wavex_sample_load_save_stop_audition(wavex_sample_load_save_page_t* page);
bool wavex_sample_load_save_load_sample(wavex_sample_load_save_page_t* page, const char* file_path);
bool wavex_sample_load_save_save_sample(wavex_sample_load_save_page_t* page, const char* file_path);

// Status and info
void wavex_sample_load_save_set_status(wavex_sample_load_save_page_t* page, const char* status);
void wavex_sample_load_save_update_info(wavex_sample_load_save_page_t* page,
                                        const wavex_file_entry_t* entry);

#endif  // WAVEX_SAMPLE_LOAD_SAVE_H
