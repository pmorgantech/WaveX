/**
 * @file sample_load_save.cpp
 * @brief Sample Load/Save Page Implementation
 */

#include "sample_load_save.h"
#include "../styles/ui_theme.h"
#include "esp_log.h"
#include <string.h>
#include <stdlib.h>
#include <cstdio>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "../../../main/comm/link_manager.h"
#include "../../../main/links/spi_link_wrapper.h"
#include "../../../shared/spi_protocol/protocol.h"
#include "../../../main/inter_mcu.h"

// LVGL includes for thread safety
#include "esp_lvgl_port.h"

// LVGL port lock macros for thread safety
#define LV_LOCK()   lvgl_port_lock(portMAX_DELAY)
#define LV_UNLOCK() lvgl_port_unlock()

static const char *TAG = "SAMPLE_LOAD_SAVE";

// Global reference to current sample load/save page for hotkey callbacks
static wavex_sample_load_save_page_t* g_current_page = NULL;

// Forward declarations
static void file_selected_callback(const wavex_file_entry_t* entry, void* user_data);
static void directory_changed_callback(const char* path, void* user_data);
static void hotkey_event_callback(lv_event_t* e);

wavex_sample_load_save_page_t* wavex_sample_load_save_create(lv_obj_t* parent)
{
    if (!parent) {
        ESP_LOGE(TAG, "Parent is NULL");
        return NULL;
    }
    
    // Clear any existing global reference to prevent conflicts
    if (g_current_page) {
        ESP_LOGW(TAG, "Clearing existing global page reference");
        g_current_page = NULL;
    }
    
    wavex_sample_load_save_page_t* page = (wavex_sample_load_save_page_t*)malloc(sizeof(wavex_sample_load_save_page_t));
    if (!page) {
        ESP_LOGE(TAG, "Failed to allocate page structure");
        return NULL;
    }
    
    // Initialize structure
    memset(page, 0, sizeof(wavex_sample_load_save_page_t));
    
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
    
    // Create real file browser with SPI communication
    ESP_LOGI(TAG, "=== SAMPLE_LOAD_SAVE 1: About to create file browser with SPI communication ===");
    ESP_LOGI(TAG, "Creating file browser with SPI communication...");
    ESP_LOGI(TAG, "=== SAMPLE_LOAD_SAVE 2: About to configure file browser ===");
    
    // Configure file browser
    wavex_file_browser_config_t browser_config = {
        .root_path = "/",
        .file_extension = ".wav",
        .max_entries = 50,
        .show_hidden = false
    };
    
    ESP_LOGI(TAG, "=== SAMPLE_LOAD_SAVE 3: About to call wavex_file_browser_create ===");
    
    page->file_browser = wavex_file_browser_create(page->file_browser_container, &browser_config);
    if (!page->file_browser) {
        ESP_LOGE(TAG, "Failed to create file browser");
        lv_obj_del(page->main_container);
        free(page);
        return NULL;
    }
    
    ESP_LOGI(TAG, "=== SAMPLE_LOAD_SAVE 4: File browser created successfully ===");
    
    // Set file browser callbacks
    wavex_file_browser_set_file_selected_callback(page->file_browser, file_selected_callback, page);
    wavex_file_browser_set_directory_changed_callback(page->file_browser, directory_changed_callback, page);
    
    ESP_LOGI(TAG, "File browser created successfully with SPI communication");
    
    // Create status label
    page->status_label = lv_label_create(page->info_panel);
    lv_label_set_text(page->status_label, "Ready - File Browser Active");
    ui_theme_apply_label_style(page->status_label, false);
    lv_obj_align(page->status_label, LV_ALIGN_TOP_LEFT, UI_PADDING_MEDIUM, UI_PADDING_MEDIUM);
    
    // Note: Hotkeys are handled by the UI task, not here
    
    // Set global reference for hotkey callbacks
    g_current_page = page;

    ESP_LOGI(TAG, "Sample Load/Save page created successfully");
    return page;
}

void wavex_sample_load_save_destroy(wavex_sample_load_save_page_t* page)
{
    if (!page) return;
    
    // Clear global reference if this is the current page
    if (g_current_page == page) {
        g_current_page = NULL;
    }
    
    // Unregister browse response callback to prevent crashes
    extern void inter_mcu_set_browse_resp_listener(wavex_browse_resp_cb_t cb, void* user_data);
    inter_mcu_set_browse_resp_listener(NULL, NULL);
    
    if (page->file_browser) {
        wavex_file_browser_destroy(page->file_browser);
    }
    
    if (page->main_container) {
        lv_obj_del(page->main_container);
    }
    
    free(page);
    ESP_LOGI(TAG, "Sample Load/Save page destroyed");
}

void wavex_sample_load_save_show(wavex_sample_load_save_page_t* page)
{
    if (!page || !page->main_container) return;
    
    LV_LOCK();
    lv_obj_clear_flag(page->main_container, LV_OBJ_FLAG_HIDDEN);
    LV_UNLOCK();
    ESP_LOGI(TAG, "Sample Load/Save page shown");
}

void wavex_sample_load_save_hide(wavex_sample_load_save_page_t* page)
{
    if (!page || !page->main_container) return;
    
    LV_LOCK();
    lv_obj_add_flag(page->main_container, LV_OBJ_FLAG_HIDDEN);
    LV_UNLOCK();
    ESP_LOGI(TAG, "Sample Load/Save page hidden");
}

void wavex_sample_load_save_update(wavex_sample_load_save_page_t* page)
{
    if (!page) return;
    
    // Update any dynamic content if needed
    // This could include updating file list, status, etc.
}

bool wavex_sample_load_save_audition_sample(wavex_sample_load_save_page_t* page, const char* file_path)
{
    if (!page || !file_path) return false;

    ESP_LOGI(TAG, "=== SAMPLE PLAY OPERATION 1: About to audition sample: %s ===", file_path);

    // Send sample play request to Daisy
    ESP_LOGI(TAG, "=== SAMPLE PLAY OPERATION 2: About to get LinkManager instance ===");
    
    LinkManager& link_mgr = LinkManager::getInstance();
    ESP_LOGI(TAG, "=== SAMPLE PLAY OPERATION 3: About to get current link ===");

    ILink* link = link_mgr.get_current_link();

    if (!link) {
        ESP_LOGE(TAG, "No link available for sample audition");
        return false;
    }

    ESP_LOGI(TAG, "=== SAMPLE PLAY OPERATION 4: About to check if link is SPI link ===");

    // Check if this is an SPI link by checking if it supports the extended methods
    // We'll use a safer approach than dynamic_cast for ESP32
    if (!link_mgr.is_spi_link()) {
        ESP_LOGE(TAG, "Link is not SPI link, cannot send sample play request");
        return false;
    }
    
    ESP_LOGI(TAG, "=== SAMPLE PLAY OPERATION 5: About to cast to SpiLink ===");

    // Cast to SpiLink - we know it's safe because is_spi_link() returned true
    SpiLink* spi_link = static_cast<SpiLink*>(link);

    ESP_LOGI(TAG, "=== SAMPLE PLAY OPERATION 6: About to call send_sample_play_req ===");

    esp_err_t result = spi_link->send_sample_play_req(file_path);
    if (result != ESP_OK) {
        ESP_LOGE(TAG, "Failed to send sample play request: %d", result);
        return false;
    }

    ESP_LOGI(TAG, "=== SAMPLE PLAY OPERATION 7: Successfully sent sample play request ===");

    page->is_playing = true;
    strncpy(page->selected_file, file_path, sizeof(page->selected_file) - 1);
    page->selected_file[sizeof(page->selected_file) - 1] = '\0';

    // Update status
    char status_text[256];
    snprintf(status_text, sizeof(status_text), "Playing: %s", file_path);
    wavex_sample_load_save_set_status(page, status_text);

    return true;
}

bool wavex_sample_load_save_stop_audition(wavex_sample_load_save_page_t* page)
{
    if (!page) return false;

    if (page->is_playing) {
        ESP_LOGI(TAG, "=== SAMPLE STOP OPERATION 1: About to stop sample audition ===");

        // Send sample stop request to Daisy
        ESP_LOGI(TAG, "=== SAMPLE STOP OPERATION 2: About to get LinkManager instance ===");
        
        LinkManager& link_mgr = LinkManager::getInstance();
        ESP_LOGI(TAG, "=== SAMPLE STOP OPERATION 3: About to get current link ===");

        ILink* link = link_mgr.get_current_link();

        if (!link) {
            ESP_LOGE(TAG, "No link available for sample stop");
            page->is_playing = false;
            wavex_sample_load_save_set_status(page, "Error: No link");
            return false;
        }

        ESP_LOGI(TAG, "=== SAMPLE STOP OPERATION 4: About to check if link is SPI link ===");

        // Check if this is an SPI link by checking if it supports the extended methods
        // We'll use a safer approach than dynamic_cast for ESP32
        if (!link_mgr.is_spi_link()) {
            ESP_LOGE(TAG, "Link is not SPI link, cannot send sample stop request");
            page->is_playing = false;
            wavex_sample_load_save_set_status(page, "Error: Not SPI link");
            return false;
        }
        
        ESP_LOGI(TAG, "=== SAMPLE STOP OPERATION 5: About to cast to SpiLink ===");
        
        // Cast to SpiLink - we know it's safe because is_spi_link() returned true
        SpiLink* spi_link = static_cast<SpiLink*>(link);

        ESP_LOGI(TAG, "=== SAMPLE STOP OPERATION 6: About to call send_sample_stop_req ===");

        esp_err_t result = spi_link->send_sample_stop_req();
        if (result != ESP_OK) {
            ESP_LOGE(TAG, "Failed to send sample stop request: %d", result);
            page->is_playing = false;
            wavex_sample_load_save_set_status(page, "Error: Stop failed");
            return false;
        }

        ESP_LOGI(TAG, "=== SAMPLE STOP OPERATION 7: Successfully sent sample stop request ===");

        page->is_playing = false;
        wavex_sample_load_save_set_status(page, "Stopped");

        return true;
    }

    return false;
}

bool wavex_sample_load_save_load_sample(wavex_sample_load_save_page_t* page, const char* file_path)
{
    if (!page || !file_path) return false;
    
    ESP_LOGI(TAG, "Loading sample: %s", file_path);
    
    // TODO: Implement actual sample loading via SPI communication with Daisy
    // This would send MSG_SAMPLE_LOAD to Daisy with the file path
    
    char status_text[256];
    snprintf(status_text, sizeof(status_text), "Loading: %s", file_path);
    wavex_sample_load_save_set_status(page, status_text);
    
    return true;
}

bool wavex_sample_load_save_save_sample(wavex_sample_load_save_page_t* page, const char* file_path)
{
    if (!page || !file_path) return false;
    
    ESP_LOGI(TAG, "Saving sample: %s", file_path);
    
    // TODO: Implement actual sample saving via SPI communication with Daisy
    // This would send appropriate save command to Daisy
    
    char status_text[256];
    snprintf(status_text, sizeof(status_text), "Saving: %s", file_path);
    wavex_sample_load_save_set_status(page, status_text);
    
    return true;
}

void wavex_sample_load_save_set_status(wavex_sample_load_save_page_t* page, const char* status)
{
    if (!page || !page->status_label || !status) return;
    
    LV_LOCK();
    lv_label_set_text(page->status_label, status);
    LV_UNLOCK();
    ESP_LOGI(TAG, "Status updated: %s", status);
}

void wavex_sample_load_save_update_info(wavex_sample_load_save_page_t* page, const wavex_file_entry_t* entry)
{
    if (!page || !entry) return;
    
    // Update info panel with file details
    // This could show file size, duration, format, etc.
    char info_text[256];
    snprintf(info_text, sizeof(info_text), 
             "File: %s\n"
             "Size: %lu bytes\n"
             "Type: %s",
             entry->name,
             entry->size_bytes,
             entry->is_directory ? "Directory" : "File");
    
    // Create or update info label
    LV_LOCK();
    lv_obj_t* info_label = lv_obj_get_child(page->info_panel, 1);
    if (!info_label) {
        info_label = lv_label_create(page->info_panel);
        ui_theme_apply_label_style(info_label, false);
        lv_obj_align(info_label, LV_ALIGN_TOP_LEFT, UI_PADDING_MEDIUM, 40);
    }
    
    lv_label_set_text(info_label, info_text);
    LV_UNLOCK();
}

// File selected callback
static void file_selected_callback(const wavex_file_entry_t* entry, void* user_data)
{
    wavex_sample_load_save_page_t* page = (wavex_sample_load_save_page_t*)user_data;
    if (!page || !entry) return;
    
    ESP_LOGI(TAG, "File selected: %s", entry->name);
    
    // Update info panel
    wavex_sample_load_save_update_info(page, entry);
    
    // Store selected file path
    strncpy(page->selected_file, entry->path, sizeof(page->selected_file) - 1);
    page->selected_file[sizeof(page->selected_file) - 1] = '\0';
}

// Directory changed callback
static void directory_changed_callback(const char* path, void* user_data)
{
    wavex_sample_load_save_page_t* page = (wavex_sample_load_save_page_t*)user_data;
    if (!page || !path) return;
    
    ESP_LOGI(TAG, "Directory changed to: %s", path);
    
    // Update status
    char status_text[256];
    snprintf(status_text, sizeof(status_text), "Browsing: %s", path);
    wavex_sample_load_save_set_status(page, status_text);
}

// Hotkey event callback
static void hotkey_event_callback(lv_event_t* e)
{
    lv_event_code_t code = lv_event_get_code(e);
    int button_index = (int)(intptr_t)lv_event_get_user_data(e);

    if (code == LV_EVENT_CLICKED && g_current_page) {
        ESP_LOGI(TAG, "Hotkey button %d clicked", button_index);

        switch (button_index) {
            case 0: // Audition
                if (g_current_page->file_browser && g_current_page->file_browser->entry_count > 0) {
                    const wavex_file_entry_t* selected = wavex_file_browser_get_selected(g_current_page->file_browser);
                    if (selected && !selected->is_directory) {
                        wavex_sample_load_save_audition_sample(g_current_page, selected->path);
                    } else {
                        ESP_LOGW(TAG, "No valid file selected for audition");
                    }
                }
                break;
            case 1: // Load
                if (g_current_page->file_browser && g_current_page->file_browser->entry_count > 0) {
                    const wavex_file_entry_t* selected = wavex_file_browser_get_selected(g_current_page->file_browser);
                    if (selected && !selected->is_directory) {
                        wavex_sample_load_save_load_sample(g_current_page, selected->path);
                    } else {
                        ESP_LOGW(TAG, "No valid file selected for loading");
                    }
                }
                break;
            case 2: // Save
                ESP_LOGI(TAG, "Save functionality not implemented yet");
                // TODO: Implement save functionality
                break;
            case 3: // Back
                ESP_LOGI(TAG, "Back hotkey pressed - navigation would be handled by UI task");
                // Navigation back to menu is handled by ui_task.cpp
                break;
            case 4: // Up (^)
                if (g_current_page->file_browser && g_current_page->file_browser->entry_count > 0) {
                    uint32_t current_idx = wavex_file_browser_get_selected_index(g_current_page->file_browser);
                    if (current_idx > 0) {
                        wavex_file_browser_set_selection(g_current_page->file_browser, current_idx - 1);
                        ESP_LOGI(TAG, "Navigated up to index %d", current_idx - 1);
                    }
                }
                break;
            case 5: // Down (v)
                if (g_current_page->file_browser && g_current_page->file_browser->entry_count > 0) {
                    uint32_t current_idx = wavex_file_browser_get_selected_index(g_current_page->file_browser);
                    if (current_idx < g_current_page->file_browser->entry_count - 1) {
                        wavex_file_browser_set_selection(g_current_page->file_browser, current_idx + 1);
                        ESP_LOGI(TAG, "Navigated down to index %d", current_idx + 1);
                    }
                }
                break;
            default:
                ESP_LOGW(TAG, "Unknown hotkey button: %d", button_index);
                break;
        }
    }
}

