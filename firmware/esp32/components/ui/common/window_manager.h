/**
 * @file window_manager.h
 * @brief Window Manager for Standardized UI Window Creation
 * 
 * This module provides standardized window creation patterns for consistent
 * UI layout across all WaveX pages.
 */

#ifndef WAVEX_WINDOW_MANAGER_H
#define WAVEX_WINDOW_MANAGER_H

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

// Window structure for standardized layout
typedef struct {
    lv_obj_t* window;        // Main window container
    lv_obj_t* header;        // Header with title
    lv_obj_t* content;       // Content area
    lv_obj_t* hotkey_region; // Hotkey buttons at bottom
    lv_obj_t* title_label;   // Title label in header
} wavex_window_t;

// Hotkey button configuration
typedef struct {
    const char* labels[6];   // Button labels (empty string to hide)
    void* user_data[6];      // User data for each button
    lv_event_cb_t callback;  // Event callback function
} wavex_hotkey_config_t;

// Window creation functions
wavex_window_t* wavex_window_create(lv_obj_t* parent, const char* title);
void wavex_window_destroy(wavex_window_t* window);

// Hotkey management
void wavex_window_set_hotkeys(wavex_window_t* window, const wavex_hotkey_config_t* config);
void wavex_window_clear_hotkeys(wavex_window_t* window);

// Content management
void wavex_window_clear_content(wavex_window_t* window);
void wavex_window_set_title(wavex_window_t* window, const char* title);

// Layout calculations
void wavex_window_update_layout(wavex_window_t* window);

#ifdef __cplusplus
}
#endif

#endif // WAVEX_WINDOW_MANAGER_H
