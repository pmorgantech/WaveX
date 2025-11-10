/**
 * @file sample_load_save.cpp
 * @brief Sample Load/Save Page Implementation
 */

#include "sample_load_save.h"

#include "../styles/ui_theme.h"

// C headers (C linkage not required for includes)
#include <stdlib.h>
#include <string.h>

#include "../include/ui/ui_navigator.h"
#include "comm/i_comm_interface.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "inter_mcu.h"
#include "ui_task.h"

#include <cstdio>

// LVGL includes for thread safety (C++)
#include "esp_lvgl_port.h"

// NOTE: LVGL locks are handled by UI task loop - all UI updates use deferred update pattern

static const char* TAG = "SAMPLE_LOAD_SAVE";
static wavex_sample_load_save_page_t* s_active_page = NULL;

// State preservation: preserve last directory path and selected index across page destroy/create
// cycles Note: Directory path is automatically restored on page creation. Selected index is
// preserved but not automatically restored (would require async coordination with browse response
// callback).
static char s_last_directory_path[96] = "/";
static uint32_t s_last_selected_index = 0;

// Forward declarations
static void file_selected_callback(const wavex_file_entry_t* entry, void* user_data);
static void file_selected_index_callback(uint32_t file_index,
                                         const wavex_file_entry_t* entry,
                                         void* user_data);
static void directory_changed_callback(const char* path, void* user_data);
static void hotkey_event_callback(lv_event_t* e);

wavex_sample_load_save_page_t* wavex_sample_load_save_create(
    lv_obj_t* parent, WaveX::Comm::ICommInterface* comm_interface) {
    if (!parent) {
        ESP_LOGE(TAG, "Parent is NULL");
        return NULL;
    }

    // Clear any existing global reference to prevent conflicts
    if (s_active_page) {
        ESP_LOGW(TAG, "Clearing existing active page reference");
        s_active_page = NULL;
    }

    wavex_sample_load_save_page_t* page =
        (wavex_sample_load_save_page_t*)malloc(sizeof(wavex_sample_load_save_page_t));
    if (!page) {
        ESP_LOGE(TAG, "Failed to allocate page structure");
        return NULL;
    }

    // Initialize structure
    memset(page, 0, sizeof(wavex_sample_load_save_page_t));
    page->pending_metadata_entry = NULL;
    page->comm_interface = comm_interface;

    // Create main container (no window manager - UI task provides titlebar and hotkeys)
    page->main_container = lv_obj_create(parent);
    lv_obj_set_size(page->main_container, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(page->main_container, UI_COLOR_CONTENT, LV_PART_MAIN);
    lv_obj_set_style_border_width(page->main_container, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(page->main_container, 0, LV_PART_MAIN);
    lv_obj_align(page->main_container, LV_ALIGN_TOP_LEFT, 0, 0);

    // Create info panel first (right side)
    page->info_panel = lv_obj_create(page->main_container);
    lv_obj_set_size(page->info_panel, lv_pct(30), lv_pct(100));
    ui_theme_apply_container_style(page->info_panel, true);
    lv_obj_align(page->info_panel, LV_ALIGN_TOP_RIGHT, -UI_PADDING_MEDIUM, UI_PADDING_MEDIUM);

    // Create file browser container (left side, 70% width)
    page->file_browser_container = lv_obj_create(page->main_container);
    lv_obj_set_size(page->file_browser_container, lv_pct(70), lv_pct(100));
    lv_obj_align(page->file_browser_container, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_style_bg_color(page->file_browser_container, UI_COLOR_CONTENT, LV_PART_MAIN);
    lv_obj_set_style_border_width(page->file_browser_container, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(page->file_browser_container, 0, LV_PART_MAIN);

    // Configure file browser - restore last directory path if available
    // This preserves user's browsing position when re-entering the page
    wavex_file_browser_config_t browser_config = {.root_path = s_last_directory_path,  // Use
                                                                                       // preserved
                                                                                       // path
                                                                                       // instead of
                                                                                       // always "/"
                                                  .file_extension = ".wav", .max_entries = 50, .show_hidden = false, .comm_interface = comm_interface};

    ESP_LOGI(TAG, "Creating file browser with root_path: %s", browser_config.root_path);

    page->file_browser = wavex_file_browser_create(page->file_browser_container, &browser_config);
    if (!page->file_browser) {
        ESP_LOGE(TAG, "Failed to create file browser");
        lv_obj_del(page->main_container);
        free(page);
        return NULL;
    }

    // Set file browser callbacks - use index-based callback for better performance
    wavex_file_browser_set_file_selected_index_callback(
        page->file_browser, file_selected_index_callback, page);
    wavex_file_browser_set_directory_changed_callback(
        page->file_browser, directory_changed_callback, page);

    // Create status label
    page->status_label = lv_label_create(page->info_panel);
    lv_label_set_text(page->status_label, "Ready - File Browser Active");
    ui_theme_apply_label_style(page->status_label, false);
    lv_obj_align(page->status_label, LV_ALIGN_TOP_LEFT, UI_PADDING_MEDIUM, UI_PADDING_MEDIUM);

    // Create metadata display area
    page->metadata_label = lv_label_create(page->info_panel);
    lv_label_set_text(page->metadata_label, "Select a file to view metadata");
    ui_theme_apply_label_style(page->metadata_label, false);
    lv_obj_align(
        page->metadata_label, LV_ALIGN_TOP_LEFT, UI_PADDING_MEDIUM, UI_PADDING_MEDIUM + 40);
    lv_obj_set_style_text_font(page->metadata_label, &lv_font_montserrat_14, LV_PART_MAIN);

    // Note: Hotkeys are handled by the UI task, not here

    // Set global reference for hotkey callbacks and deferred updates
    s_active_page = page;

    ESP_LOGI(TAG, "Sample Load/Save page created successfully");
    return page;
}

void wavex_sample_load_save_destroy(wavex_sample_load_save_page_t* page) {
    if (!page)
        return;

    // Stop any active audition before destroying the page
    // This prevents the Daisy audio co-processor from continuing playback after page destruction
    if (page->is_playing) {
        ESP_LOGI(TAG, "Stopping active audition before page destruction");
        wavex_sample_load_save_stop_audition(page);

        // Wait a brief moment for the stop command to be sent
        // Note: We don't wait for the response since the page is being destroyed
        vTaskDelay(pdMS_TO_TICKS(50));
    }

    // Save browsing state before destroying file browser
    if (page->file_browser) {
        const char* current_path = wavex_file_browser_get_current_path(page->file_browser);
        if (current_path && strlen(current_path) > 0) {
            strncpy(s_last_directory_path, current_path, sizeof(s_last_directory_path) - 1);
            s_last_directory_path[sizeof(s_last_directory_path) - 1] = '\0';
            ESP_LOGI(TAG, "Preserved directory path: %s", s_last_directory_path);
        }

        uint32_t selected_index = wavex_file_browser_get_selected_index(page->file_browser);
        s_last_selected_index = selected_index;
        ESP_LOGI(TAG, "Preserved selected index: %lu", (unsigned long)s_last_selected_index);
    }

    // Clear global reference if this is the current page
    if (s_active_page == page) {
        s_active_page = NULL;
    }

    // Unregister browse response callback to prevent crashes
    if (page->comm_interface) {
        page->comm_interface->setBrowseResponseListener(NULL, NULL);
    }

    if (page->file_browser) {
        wavex_file_browser_destroy(page->file_browser);
    }

    if (page->main_container) {
        lv_obj_del(page->main_container);
    }

    free(page);
    ESP_LOGI(TAG, "Sample Load/Save page destroyed");
}

wavex_sample_load_save_page_t* wavex_sample_load_save_get_active(void) {
    return s_active_page;
}

void wavex_sample_load_save_show(wavex_sample_load_save_page_t* page) {
    if (!page || !page->main_container)
        return;

    // Page show/hide typically called from LVGL context (navigator), so direct update is safe
    // But to be consistent with deferred update pattern, we'll use direct update here since
    // navigator holds lock
    lv_obj_clear_flag(page->main_container, LV_OBJ_FLAG_HIDDEN);
    ESP_LOGI(TAG, "Sample Load/Save page shown");
}

void wavex_sample_load_save_hide(wavex_sample_load_save_page_t* page) {
    if (!page || !page->main_container)
        return;

    // Page show/hide typically called from LVGL context (navigator), so direct update is safe
    // But to be consistent with deferred update pattern, we'll use direct update here since
    // navigator holds lock
    lv_obj_add_flag(page->main_container, LV_OBJ_FLAG_HIDDEN);
    ESP_LOGI(TAG, "Sample Load/Save page hidden");
}

// Process deferred UI updates (must be called with LVGL lock held)
static void process_deferred_page_updates(wavex_sample_load_save_page_t* page) {
    if (!page)
        return;

    // Process status update
    if (page->status_update_pending && page->status_label) {
        lv_label_set_text(page->status_label, page->pending_status_text);
        page->status_update_pending = false;
        ESP_LOGI(TAG, "Status label updated: %s", page->pending_status_text);
    }

    // Process metadata update
    if (page->metadata_update_pending && page->metadata_label) {
        char info_text[512];

        if (page->pending_metadata_entry) {
            const wavex_file_entry_t* entry = page->pending_metadata_entry;

            if (entry->is_directory) {
                // Display directory information
                snprintf(info_text,
                         sizeof(info_text),
                         "Directory Information:\n"
                         "─────────────────────\n"
                         "Name: %.47s\n"
                         "Type: Directory\n"
                         "Path: %.95s\n\n"
                         "Use Select to enter directory",
                         entry->name,
                         entry->path);
            } else {
                // Display file metadata
                // Format file size
                char size_str[32];
                if (entry->size_bytes < 1024) {
                    snprintf(size_str, sizeof(size_str), "%lu B", entry->size_bytes);
                } else if (entry->size_bytes < 1024 * 1024) {
                    snprintf(size_str, sizeof(size_str), "%.1f KB", entry->size_bytes / 1024.0f);
                } else {
                    snprintf(size_str,
                             sizeof(size_str),
                             "%.1f MB",
                             entry->size_bytes / (1024.0f * 1024.0f));
                }

                // Format duration if available (placeholder for now)
                char duration_str[32] = "Unknown";
                // TODO: Extract duration from WAV file metadata

                // Format sample rate if available (placeholder for now)
                char sample_rate_str[32] = "Unknown";
                // TODO: Extract sample rate from WAV file metadata

                // Format channels if available (placeholder for now)
                char channels_str[32] = "Unknown";
                // TODO: Extract channel count from WAV file metadata

                snprintf(info_text,
                         sizeof(info_text),
                         "Sample Information:\n"
                         "─────────────────\n"
                         "Name: %.47s\n"
                         "Size: %s\n"
                         "Duration: %s\n"
                         "Sample Rate: %s\n"
                         "Channels: %s\n"
                         "Format: WAV PCM\n"
                         "Path: %.95s\n\n"
                         "Use Audition to preview\n"
                         "Use Load to load sample",
                         entry->name,
                         size_str,
                         duration_str,
                         sample_rate_str,
                         channels_str,
                         entry->path);
            }

            lv_label_set_text(page->metadata_label, info_text);
            page->metadata_update_pending = false;
            page->pending_metadata_entry = NULL;
            ESP_LOGI(TAG, "Metadata label updated for: %s", entry->name);
        } else if (strlen(page->pending_metadata_text) > 0) {
            // Use pending text (for "Select a file" message)
            lv_label_set_text(page->metadata_label, page->pending_metadata_text);
            page->metadata_update_pending = false;
            page->pending_metadata_text[0] = '\0';
            ESP_LOGI(TAG, "Metadata label updated with pending text");
        }
    }
}

void wavex_sample_load_save_update(wavex_sample_load_save_page_t* page) {
    ESP_LOGD(TAG, "wavex_sample_load_save_update called");
    if (!page)
        return;

    // Process any pending file browser UI updates (thread-safe)
    if (page->file_browser) {
        // Check if there's a pending update (for debugging)
        if (page->file_browser->ui_update_pending) {
            ESP_LOGI(TAG, "Processing pending file browser UI update...");
        }
        wavex_file_browser_process_pending_updates(page->file_browser);
    }

    // Process deferred page updates (status/metadata)
    process_deferred_page_updates(page);
}

bool wavex_sample_load_save_audition_sample_by_index(wavex_sample_load_save_page_t* page,
                                                     uint32_t file_index) {
    if (!page)
        return false;

    ESP_LOGI(TAG,
             "=== SAMPLE PLAY INDEX OPERATION: About to audition sample by index: %lu ===",
             (unsigned long)file_index);

    esp_err_t result =
        page->comm_interface ? page->comm_interface->sendSamplePlayRequest(file_index) : ESP_FAIL;
    if (result != ESP_OK) {
        ESP_LOGE(TAG, "Failed to send sample play index request: %d", result);
        return false;
    }

    ESP_LOGI(TAG, "=== SAMPLE PLAY INDEX OPERATION: Request sent successfully ===");
    page->is_playing = true;
    snprintf(
        page->selected_file, sizeof(page->selected_file), "Index %lu", (unsigned long)file_index);

    // Update status
    char status_text[256];
    snprintf(status_text, sizeof(status_text), "Playing: Index %lu", (unsigned long)file_index);
    wavex_sample_load_save_set_status(page, status_text);

    // Refresh softkeys to reflect playing state (Audition -> Stop)
    // This must be called from LVGL context via lv_async_call
    wavex_ui_mark_content_changed();
    lv_async_call([](void*) { wavex_ui::UINavigator::instance().refreshSoftkeys(); }, nullptr);

    return true;
}

bool wavex_sample_load_save_stop_audition(wavex_sample_load_save_page_t* page) {
    if (!page)
        return false;

    if (page->is_playing) {
        ESP_LOGI(TAG, "=== SAMPLE STOP OPERATION: Stopping sample ===");

        esp_err_t result =
            page->comm_interface ? page->comm_interface->sendSampleStopRequest() : ESP_FAIL;
        if (result != ESP_OK) {
            ESP_LOGE(TAG, "Failed to send sample stop request: %d", result);
            page->is_playing = false;
            wavex_sample_load_save_set_status(page, "Error: Stop failed");
            return false;
        }

        ESP_LOGI(TAG, "=== SAMPLE STOP OPERATION: Request sent successfully ===");

        // Don't immediately change is_playing - wait for response
        // Show "Stopping..." status instead of immediate UI change
        wavex_sample_load_save_set_status(page, "Stopping...");

        return true;
    }

    return false;
}

// Handle sample stop response from Daisy
void wavex_ui_handle_sample_stop_response(bool success) {
    // Access the global sample load/save page instance
    if (!s_active_page) {
        ESP_LOGW(TAG, "Received sample stop response but no active sample load/save page");
        return;
    }

    if (success) {
        ESP_LOGI(TAG, "=== SAMPLE STOP RESPONSE: Successfully stopped ===");
        s_active_page->is_playing = false;
        wavex_sample_load_save_set_status(s_active_page, "Stopped");

        // Refresh softkeys to reflect stopped state (Stop -> Audition)
        // This must be called from LVGL context via lv_async_call
        wavex_ui_mark_content_changed();
        lv_async_call([](void*) { wavex_ui::UINavigator::instance().refreshSoftkeys(); }, nullptr);
    } else {
        ESP_LOGE(TAG, "=== SAMPLE STOP RESPONSE: Failed to stop ===");
        wavex_sample_load_save_set_status(s_active_page, "Stop failed");
        // Keep is_playing state as-is since stop failed
    }
}

bool wavex_sample_load_save_load_sample(wavex_sample_load_save_page_t* page,
                                        const char* file_path) {
    if (!page || !file_path)
        return false;

    ESP_LOGI(TAG, "Loading sample: %s", file_path);

    // TODO: Implement actual sample loading via SPI communication with Daisy
    // This would send MSG_SAMPLE_LOAD to Daisy with the file path

    char status_text[256];
    snprintf(status_text, sizeof(status_text), "Loading: %s", file_path);
    wavex_sample_load_save_set_status(page, status_text);

    return true;
}

bool wavex_sample_load_save_save_sample(wavex_sample_load_save_page_t* page,
                                        const char* file_path) {
    if (!page || !file_path)
        return false;

    ESP_LOGI(TAG, "Saving sample: %s", file_path);

    // TODO: Implement actual sample saving via SPI communication with Daisy
    // This would send appropriate save command to Daisy

    char status_text[256];
    snprintf(status_text, sizeof(status_text), "Saving: %s", file_path);
    wavex_sample_load_save_set_status(page, status_text);

    return true;
}

void wavex_sample_load_save_set_status(wavex_sample_load_save_page_t* page, const char* status) {
    if (!page || !status)
        return;

    // Store status text for deferred update (may be called from non-LVGL context)
    strncpy(page->pending_status_text, status, sizeof(page->pending_status_text) - 1);
    page->pending_status_text[sizeof(page->pending_status_text) - 1] = '\0';
    page->status_update_pending = true;
    wavex_ui_mark_content_changed();
    ESP_LOGI(TAG, "Status update queued: %s", status);
}

void wavex_sample_load_save_update_info(wavex_sample_load_save_page_t* page,
                                        const wavex_file_entry_t* entry) {
    if (!page || !entry)
        return;

    // Store entry pointer for deferred update (may be called from non-LVGL context)
    page->pending_metadata_entry = entry;
    page->metadata_update_pending = true;
    wavex_ui_mark_content_changed();
    ESP_LOGI(TAG, "Metadata update queued for: %s", entry->name);
}

// File selected callback
static void file_selected_callback(const wavex_file_entry_t* entry, void* user_data) {
    wavex_sample_load_save_page_t* page = (wavex_sample_load_save_page_t*)user_data;
    if (!page || !entry)
        return;

    ESP_LOGI(TAG, "File selected: %s", entry->name);

    // Update info panel
    wavex_sample_load_save_update_info(page, entry);

    // Store selected file path
    strncpy(page->selected_file, entry->path, sizeof(page->selected_file) - 1);
    page->selected_file[sizeof(page->selected_file) - 1] = '\0';
}

static void file_selected_index_callback(uint32_t file_index,
                                         const wavex_file_entry_t* entry,
                                         void* user_data) {
    ESP_LOGI(TAG,
             "=== INDEX CALLBACK CALLED: index=%lu, entry=%p, user_data=%p ===",
             (unsigned long)file_index,
             entry,
             user_data);

    wavex_sample_load_save_page_t* page = (wavex_sample_load_save_page_t*)user_data;
    if (!page || !entry) {
        ESP_LOGE(TAG, "Invalid parameters: page=%p, entry=%p", page, entry);
        return;
    }

    ESP_LOGI(TAG, "File selected by index %lu: %s", (unsigned long)file_index, entry->name);

    // Update info panel
    wavex_sample_load_save_update_info(page, entry);

    // Store selected file index and name (not full path to avoid length issues)
    page->selected_file_index = file_index;
    strncpy(page->selected_file, entry->name, sizeof(page->selected_file) - 1);
    page->selected_file[sizeof(page->selected_file) - 1] = '\0';

    ESP_LOGI(TAG,
             "Stored selected_file_index=%lu, selected_file='%s'",
             (unsigned long)page->selected_file_index,
             page->selected_file);
}

// Directory changed callback
static void directory_changed_callback(const char* path, void* user_data) {
    wavex_sample_load_save_page_t* page = (wavex_sample_load_save_page_t*)user_data;
    if (!page || !path)
        return;

    ESP_LOGI(TAG, "Directory changed to: %s", path);

    // Clear previous state to prevent sync issues
    page->selected_file_index = 0;
    memset(page->selected_file, 0, sizeof(page->selected_file));

    // Stop any current audition when changing directories
    if (page->is_playing) {
        ESP_LOGI(TAG, "Stopping audition due to directory change");

        // Send stop request to Daisy
        if (page->comm_interface) {
            page->comm_interface->sendSampleStopRequest();
        }

        // Immediately reset playing state (don't wait for response since we're changing directories
        // anyway)
        page->is_playing = false;

        // Refresh softkeys to show Audition button again instead of Stop
        // Use lv_async_call to ensure we're in LVGL context
        lv_async_call([](void*) { wavex_ui::UINavigator::instance().refreshSoftkeys(); }, nullptr);
    }

    // Clear metadata display (deferred update)
    strcpy(page->pending_metadata_text, "Select a file to view metadata");
    page->metadata_update_pending = true;
    page->pending_metadata_entry = NULL;
    wavex_ui_mark_content_changed();

    // Update status
    char status_text[256];
    snprintf(status_text, sizeof(status_text), "Browsing: %s", path);
    wavex_sample_load_save_set_status(page, status_text);
}

// Note: Hotkey handling is now done by ui_task.cpp to ensure proper navigation
// This allows the UI task to handle back navigation and other screen transitions
