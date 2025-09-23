/**
 * @file sample_load_save.h
 * @brief Sample Load/Save Page Implementation
 * 
 * This page provides file browsing and sample audition functionality
 * for loading and saving audio samples from the SD card.
 */

#ifndef WAVEX_SAMPLE_LOAD_SAVE_H
#define WAVEX_SAMPLE_LOAD_SAVE_H

#include "lvgl.h"
#include "../common/window_manager.h"
#include "../components/file_browser.h"

#ifdef __cplusplus
extern "C" {
#endif

// Sample load/save page structure
typedef struct {
    lv_obj_t* main_container;
    lv_obj_t* file_browser_container;
    wavex_file_browser_t* file_browser;
    lv_obj_t* info_panel;
    lv_obj_t* status_label;
    bool is_playing;
    char selected_file[96];
    uint32_t selected_file_index;
} wavex_sample_load_save_page_t;

// Page creation and management
wavex_sample_load_save_page_t* wavex_sample_load_save_create(lv_obj_t* parent);
void wavex_sample_load_save_destroy(wavex_sample_load_save_page_t* page);

// Page interface functions
void wavex_sample_load_save_show(wavex_sample_load_save_page_t* page);
void wavex_sample_load_save_hide(wavex_sample_load_save_page_t* page);
void wavex_sample_load_save_update(wavex_sample_load_save_page_t* page);

// Sample operations
bool wavex_sample_load_save_audition_sample(wavex_sample_load_save_page_t* page, const char* file_path);
bool wavex_sample_load_save_audition_sample_by_index(wavex_sample_load_save_page_t* page, uint32_t file_index);
bool wavex_sample_load_save_stop_audition(wavex_sample_load_save_page_t* page);
bool wavex_sample_load_save_load_sample(wavex_sample_load_save_page_t* page, const char* file_path);
bool wavex_sample_load_save_save_sample(wavex_sample_load_save_page_t* page, const char* file_path);

// Status and info
void wavex_sample_load_save_set_status(wavex_sample_load_save_page_t* page, const char* status);
void wavex_sample_load_save_update_info(wavex_sample_load_save_page_t* page, const wavex_file_entry_t* entry);

#ifdef __cplusplus
}
#endif

#endif // WAVEX_SAMPLE_LOAD_SAVE_H
