/**
 * @file file_browser.cpp
 * @brief File Browser Component Implementation
 */

#include "file_browser.h"
#include "../styles/ui_theme.h"
#include "esp_log.h"
#include <string.h>
#include <stdlib.h>
#include <cstdio>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "../../../../shared/spi_protocol/protocol.h"
#include "../../../main/comm/link_manager.h"
#include "../../../main/links/spi_link_wrapper.h"
#include "../../../../shared/config/link_config.h"
#include "../../../main/inter_mcu.h"

// LVGL includes for thread safety
#include "esp_lvgl_port.h"

// LVGL port lock macros for thread safety
#define LV_LOCK()   lvgl_port_lock(portMAX_DELAY)
#define LV_UNLOCK() lvgl_port_unlock()

static const char *TAG = "FILE_BROWSER";

// Global reference to current file browser for callback
static wavex_file_browser_t* g_current_browser = NULL;

// Forward declarations
static void file_list_event_cb(lv_event_t *e);
static bool refresh_file_list(wavex_file_browser_t* browser);
static bool parse_browse_response(const uint8_t* data, size_t length, wavex_file_entry_t* entries, uint32_t* count);
static bool send_browse_request(const char* path);
static void update_visual_selection(wavex_file_browser_t* browser);
static void browse_resp_callback(const uint8_t* data, size_t length, void* user_data);

wavex_file_browser_t* wavex_file_browser_create(lv_obj_t* parent, const wavex_file_browser_config_t* config)
{
    ESP_LOGI(TAG, "=== FILE_BROWSER_CREATE 1: About to validate parameters ===");
    
    if (!parent || !config) {
        ESP_LOGE(TAG, "Invalid parameters");
        return NULL;
    }
    
    ESP_LOGI(TAG, "=== FILE_BROWSER_CREATE 2: About to allocate browser structure ===");
    
    wavex_file_browser_t* browser = (wavex_file_browser_t*)malloc(sizeof(wavex_file_browser_t));
    if (!browser) {
        ESP_LOGE(TAG, "Failed to allocate file browser structure");
        return NULL;
    }
    
    ESP_LOGI(TAG, "=== FILE_BROWSER_CREATE 3: About to initialize structure ===");

    // Initialize structure
    memset(browser, 0, sizeof(wavex_file_browser_t));
    browser->config = *config;
    browser->selected_index = 0;
    
    ESP_LOGI(TAG, "=== FILE_BROWSER_CREATE 4: About to copy root path ===");
    
    // Copy root path
    strncpy(browser->current_path, config->root_path ? config->root_path : "/", sizeof(browser->current_path) - 1);
    browser->current_path[sizeof(browser->current_path) - 1] = '\0';
    
    ESP_LOGI(TAG, "=== FILE_BROWSER_CREATE 5: About to allocate entries array ===");
    
    // Allocate entries array
    browser->entries = (wavex_file_entry_t*)malloc(config->max_entries * sizeof(wavex_file_entry_t));
    if (!browser->entries) {
        ESP_LOGE(TAG, "Failed to allocate entries array");
        free(browser);
        return NULL;
    }
    
    ESP_LOGI(TAG, "=== FILE_BROWSER_CREATE 6: About to create main container ===");
    
    // Create main container
    LV_LOCK();
    browser->container = lv_obj_create(parent);
    lv_obj_set_size(browser->container, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(browser->container, UI_COLOR_CONTENT, LV_PART_MAIN);
    lv_obj_set_style_border_width(browser->container, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(browser->container, 0, LV_PART_MAIN);
    lv_obj_align(browser->container, LV_ALIGN_TOP_LEFT, 0, 0);
    
    ESP_LOGI(TAG, "=== FILE_BROWSER_CREATE 7: About to create path label ===");
    
    // Create path label
    browser->path_label = lv_label_create(browser->container);
    lv_label_set_text(browser->path_label, browser->current_path);
    ui_theme_apply_label_style(browser->path_label, false);
    lv_obj_align(browser->path_label, LV_ALIGN_TOP_LEFT, UI_PADDING_MEDIUM, UI_PADDING_MEDIUM);
    
    ESP_LOGI(TAG, "=== FILE_BROWSER_CREATE 8: About to create file list ===");
    
    // Create file list
    browser->list = lv_list_create(browser->container);
    lv_obj_set_size(browser->list, lv_pct(100), lv_pct(100) - 40); // Leave space for path label
    lv_obj_align(browser->list, LV_ALIGN_TOP_LEFT, 0, 40);
    lv_obj_set_style_bg_color(browser->list, UI_COLOR_CONTENT, LV_PART_MAIN);
    lv_obj_set_style_border_width(browser->list, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(browser->list, UI_PADDING_SMALL, LV_PART_MAIN);
    
    ESP_LOGI(TAG, "=== FILE_BROWSER_CREATE 9: About to add event callback ===");
    
    // Add event callback for list
    lv_obj_add_event_cb(browser->list, file_list_event_cb, LV_EVENT_CLICKED, browser);
    LV_UNLOCK();
    
    ESP_LOGI(TAG, "=== FILE_BROWSER_CREATE 10: About to set global reference ===");
    
    // Set global reference and register callback
    g_current_browser = browser;
    
    ESP_LOGI(TAG, "=== FILE_BROWSER_CREATE 11: About to register browse response callback ===");

    // Register browse response callback with inter-MCU system
    extern void inter_mcu_set_browse_resp_listener(wavex_browse_resp_cb_t cb, void* user_data);
    inter_mcu_set_browse_resp_listener(browse_resp_callback, browser);
    
    ESP_LOGI(TAG, "=== FILE_BROWSER_CREATE 12: About to refresh file list ===");
    
    // Refresh file list
    ESP_LOGI(TAG, "Calling refresh_file_list for path: %s", browser->current_path);
    if (!refresh_file_list(browser)) {
        ESP_LOGE(TAG, "Failed to refresh file list");
    }
    
    ESP_LOGI(TAG, "=== FILE_BROWSER_CREATE 13: File browser created successfully ===");

    ESP_LOGI(TAG, "File browser created for path: %s with %d entries", browser->current_path, browser->entry_count);
    
    // Log all file entries for debugging
    for (uint32_t i = 0; i < browser->entry_count; i++) {
        ESP_LOGI(TAG, "Entry %d: %s (%s) - %s", i, 
                browser->entries[i].name,
                browser->entries[i].is_directory ? "DIR" : "FILE",
                browser->entries[i].path);
    }
    return browser;
}

void wavex_file_browser_destroy(wavex_file_browser_t* browser)
{
    if (!browser) return;
    
    // Clear global reference if this is the current browser
    if (g_current_browser == browser) {
        g_current_browser = NULL;
    }
    
    if (browser->entries) {
        free(browser->entries);
    }
    
    if (browser->container) {
        LV_LOCK();
        lv_obj_del(browser->container);
        LV_UNLOCK();
    }
    
    free(browser);
    ESP_LOGI(TAG, "File browser destroyed");
}

bool wavex_file_browser_navigate_to(wavex_file_browser_t* browser, const char* path)
{
    if (!browser || !path) return false;
    
    strncpy(browser->current_path, path, sizeof(browser->current_path) - 1);
    browser->current_path[sizeof(browser->current_path) - 1] = '\0';
    
    // Update path label
    LV_LOCK();
    lv_label_set_text(browser->path_label, browser->current_path);
    LV_UNLOCK();
    
    // Refresh file list
    bool success = refresh_file_list(browser);
    
    // Notify directory changed callback
    if (success && browser->dir_changed_cb) {
        browser->dir_changed_cb(browser->current_path, browser->user_data);
    }
    
    return success;
}

bool wavex_file_browser_navigate_up(wavex_file_browser_t* browser)
{
    if (!browser) return false;
    
    // Find last directory separator
    char* last_slash = strrchr(browser->current_path, '/');
    if (last_slash && last_slash != browser->current_path) {
        *last_slash = '\0';
    } else {
        strcpy(browser->current_path, "/");
    }
    
    return wavex_file_browser_navigate_to(browser, browser->current_path);
}

bool wavex_file_browser_refresh(wavex_file_browser_t* browser)
{
    if (!browser) return false;
    
    return refresh_file_list(browser);
}

void wavex_file_browser_set_selection(wavex_file_browser_t* browser, uint32_t index)
{
    if (!browser || index >= browser->entry_count) return;
    
    browser->selected_index = index;
    ESP_LOGI(TAG, "Selected entry %d: %s", index, browser->entries[index].name);
    
    // Update visual highlighting
    update_visual_selection(browser);
}

const wavex_file_entry_t* wavex_file_browser_get_selected(wavex_file_browser_t* browser)
{
    if (!browser || browser->selected_index >= browser->entry_count) return NULL;
    
    return &browser->entries[browser->selected_index];
}

uint32_t wavex_file_browser_get_selected_index(wavex_file_browser_t* browser)
{
    if (!browser) return 0;
    
    return browser->selected_index;
}

void wavex_file_browser_set_file_selected_callback(wavex_file_browser_t* browser, 
                                                   wavex_file_selected_cb_t callback, 
                                                   void* user_data)
{
    if (!browser) return;
    
    browser->file_selected_cb = callback;
    browser->user_data = user_data;
}

void wavex_file_browser_set_directory_changed_callback(wavex_file_browser_t* browser, 
                                                       wavex_directory_changed_cb_t callback, 
                                                       void* user_data)
{
    if (!browser) return;
    
    browser->dir_changed_cb = callback;
    browser->user_data = user_data;
}

const char* wavex_file_browser_get_current_path(wavex_file_browser_t* browser)
{
    if (!browser) return NULL;
    
    return browser->current_path;
}

uint32_t wavex_file_browser_get_entry_count(wavex_file_browser_t* browser)
{
    if (!browser) return 0;
    
    return browser->entry_count;
}

const wavex_file_entry_t* wavex_file_browser_get_entry(wavex_file_browser_t* browser, uint32_t index)
{
    if (!browser || index >= browser->entry_count) return NULL;
    
    return &browser->entries[index];
}

// File list event callback
static void file_list_event_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    wavex_file_browser_t* browser = (wavex_file_browser_t*)lv_event_get_user_data(e);
    
    if (code == LV_EVENT_CLICKED) {
        lv_obj_t* list = (lv_obj_t*)lv_event_get_target(e);
        lv_obj_t* btn = (lv_obj_t*)lv_event_get_target(e);
        
        // Find the button index by iterating through the list
        uint32_t index = 0;
        lv_obj_t* child = lv_obj_get_child(list, 0);
        while (child && child != btn) {
            child = lv_obj_get_child(list, index + 1);
            index++;
        }
        
        if (index < browser->entry_count) {
            wavex_file_browser_set_selection(browser, index);
            
            // If it's a directory, navigate into it
            if (browser->entries[index].is_directory) {
                char new_path[256];
                snprintf(new_path, sizeof(new_path), "%s/%s", browser->current_path, browser->entries[index].name);
                wavex_file_browser_navigate_to(browser, new_path);
            } else {
                // File selected - notify callback
                if (browser->file_selected_cb) {
                    browser->file_selected_cb(&browser->entries[index], browser->user_data);
                }
            }
        }
    }
}

// Refresh file list by requesting directory contents from Daisy
static bool refresh_file_list(wavex_file_browser_t* browser)
{
    if (!browser) return false;
    
    ESP_LOGI(TAG, "refresh_file_list called for path: %s", browser->current_path);
    
    // Clear existing list items
    LV_LOCK();
    lv_obj_clean(browser->list);
    LV_UNLOCK();
    browser->entry_count = 0;
    
    // Send browse request to Daisy
    if (!send_browse_request(browser->current_path)) {
        ESP_LOGE(TAG, "Failed to send browse request");
        // Show error message
        LV_LOCK();
        lv_obj_clean(browser->list);
        lv_obj_t* btn = lv_list_add_btn(browser->list, NULL, "Error loading files");
        ui_theme_apply_button_style(btn, false);
        lv_obj_set_style_text_color(btn, UI_COLOR_TEXT, LV_PART_MAIN);
        lv_obj_set_style_text_font(btn, &lv_font_montserrat_18, LV_PART_MAIN);
        LV_UNLOCK();
        return false;
    } else {
        ESP_LOGI(TAG, "Sent browse request, waiting for response...");
        // The browse_resp_callback will handle the response and call refresh_file_list again
        return true;
    }
    
    ESP_LOGI(TAG, "Refreshed file list with %d entries", browser->entry_count);
    return true;
}

// Parse browse response from Daisy
static bool parse_browse_response(const uint8_t* data, size_t length, wavex_file_entry_t* entries, uint32_t* count)
{
    if (!data || length < sizeof(WaveX::Protocol::PacketHeader) + sizeof(WaveX::Protocol::BrowseRespHeader)) {
        ESP_LOGE(TAG, "Browse response too short: %d bytes", (int)length);
        *count = 0;
        return false;
    }

    // Parse as PacketHeader + payload format (not Packet structure)
    const WaveX::Protocol::PacketHeader* header = (const WaveX::Protocol::PacketHeader*)data;
    if (header->type != WaveX::Protocol::MSG_BROWSE_RESP) {
        ESP_LOGE(TAG, "Wrong message type: expected %d, got %d", WaveX::Protocol::MSG_BROWSE_RESP, header->type);
        *count = 0;
        return false;
    }

    const uint8_t* payload = data + sizeof(WaveX::Protocol::PacketHeader);
    const WaveX::Protocol::BrowseRespHeader* browse_header = (const WaveX::Protocol::BrowseRespHeader*)payload;
    uint32_t total_count = browse_header->total_count;
    uint8_t n_entries = browse_header->n;

    ESP_LOGI(TAG, "Browse response: total_count=%lu, n_entries=%u", (unsigned long)total_count, n_entries);
    
    // Debug: Show raw payload data
    ESP_LOGI(TAG, "Raw payload (first 64 bytes):");
    for (int i = 0; i < 64 && i < (int)(length - sizeof(WaveX::Protocol::PacketHeader)); i++) {
        if (i % 16 == 0) ESP_LOGI(TAG, "");
        ESP_LOGI(TAG, "%02X ", payload[i]);
    }
    ESP_LOGI(TAG, "");

    // Parse file entries
    uint32_t parsed_count = 0;
    const WaveX::Protocol::FileEntryWire* wire_entries = (const WaveX::Protocol::FileEntryWire*)(payload + sizeof(WaveX::Protocol::BrowseRespHeader));

    ESP_LOGI(TAG, "Starting file entry parsing: n_entries=%u, max_count=%u", n_entries, *count);
    ESP_LOGI(TAG, "Wire entries pointer: %p, payload offset: %d", wire_entries, (int)(payload - data + sizeof(WaveX::Protocol::BrowseRespHeader)));

    for (uint8_t i = 0; i < n_entries && parsed_count < *count; i++) {
        const WaveX::Protocol::FileEntryWire* wire = &wire_entries[i];
        
        ESP_LOGI(TAG, "Parsing entry %d: is_dir=%d, size=%lu, name='%.50s'", 
                 i, wire->is_dir, (unsigned long)wire->size_bytes, wire->name);

        // Copy to our file entry structure
        wavex_file_entry_t* entry = &entries[parsed_count++];
        entry->is_directory = wire->is_dir;
        entry->size_bytes = wire->size_bytes;
        strncpy(entry->name, wire->name, sizeof(entry->name) - 1);
        entry->name[sizeof(entry->name) - 1] = '\0';

        // Create full path - for now just use the name, full path will be constructed when needed
        strncpy(entry->path, entry->name, sizeof(entry->path) - 1);
        entry->path[sizeof(entry->path) - 1] = '\0';
        
        ESP_LOGI(TAG, "Parsed entry %d: '%s' (%s) - %lu bytes", 
                 i, entry->name, entry->is_directory ? "DIR" : "FILE", (unsigned long)entry->size_bytes);
    }

    *count = parsed_count;
    return true;
}

// Send browse request to Daisy
static bool send_browse_request(const char* path)
{
    ESP_LOGI(TAG, "=== SPI OPERATION 1: About to get LinkManager instance ===");
    
    LinkManager& link_mgr = LinkManager::getInstance();
    ESP_LOGI(TAG, "=== SPI OPERATION 2: About to get current link ===");

    ILink* link = link_mgr.get_current_link();

    if (!link) {
        ESP_LOGE(TAG, "No link available for browse request");
        return false;
    }

    ESP_LOGI(TAG, "=== SPI OPERATION 3: About to check if link is SPI link ===");

    // Check if this is an SPI link by checking if it supports the extended methods
    // We'll use a safer approach than dynamic_cast for ESP32
    if (!link_mgr.is_spi_link()) {
        ESP_LOGE(TAG, "Link is not SPI link, cannot send browse request");
        return false;
    }
    
    ESP_LOGI(TAG, "=== SPI OPERATION 4: About to cast to SpiLink ===");
    
    // Cast to SpiLink - we know it's safe because is_spi_link() returned true
    SpiLink* spi_link = static_cast<SpiLink*>(link);

    ESP_LOGI(TAG, "=== SPI OPERATION 5: About to call send_browse_req ===");

    esp_err_t result = spi_link->send_browse_req(path);
    if (result != ESP_OK) {
        ESP_LOGE(TAG, "Failed to send browse request: %d", result);
        return false;
    }

    ESP_LOGI(TAG, "=== SPI OPERATION 6: Successfully sent browse request for path: %s ===", path);
    return true;
}


// Browse response callback function
static void browse_resp_callback(const uint8_t* data, size_t length, void* user_data)
{
    wavex_file_browser_t* browser = (wavex_file_browser_t*)user_data;
    if (!browser || !data || length == 0) {
        ESP_LOGE(TAG, "Invalid browse response callback parameters");
        return;
    }
    
    ESP_LOGI(TAG, "Received browse response: %d bytes", (int)length);
    
    // Log raw data for debugging (first 64 bytes)
    ESP_LOGI(TAG, "Raw data (first 64 bytes):");
    for (int i = 0; i < (int)length && i < 64; i++) {
        if (i % 16 == 0) {
            ESP_LOGI(TAG, "  %04X: ", i);
        }
        ESP_LOGI(TAG, "%02X ", data[i]);
        if (i % 16 == 15) {
            ESP_LOGI(TAG, "");
        }
    }
    if (length % 16 != 0) {
        ESP_LOGI(TAG, "");
    }
    
    // Parse the browse response
    uint32_t entry_count = browser->config.max_entries;  // Pass max capacity, not 0
    if (parse_browse_response(data, length, browser->entries, &entry_count)) {
        browser->entry_count = entry_count;
        ESP_LOGI(TAG, "Parsed %d file entries from browse response", entry_count);
        
        // Log all parsed entries for debugging
        for (uint32_t i = 0; i < entry_count; i++) {
            ESP_LOGI(TAG, "Parsed Entry %d: %s (%s) - %s", i, 
                    browser->entries[i].name,
                    browser->entries[i].is_directory ? "DIR" : "FILE",
                    browser->entries[i].path);
        }
        
        // Clear existing list items
        LV_LOCK();
        lv_obj_clean(browser->list);
        
        if (browser->entry_count > 0) {
            // Create list items
            for (uint32_t i = 0; i < browser->entry_count; i++) {
                lv_obj_t* btn = lv_list_add_btn(browser->list, NULL, browser->entries[i].name);
                
                // Apply styling
                ui_theme_apply_button_style(btn, true);
                
                // Set text color to white and increase font size to 18px
                lv_obj_set_style_text_color(btn, UI_COLOR_TEXT, LV_PART_MAIN);
                lv_obj_set_style_text_font(btn, UI_FONT_TITLE, LV_PART_MAIN);
                
                // Add directory indicator
                if (browser->entries[i].is_directory) {
                    lv_obj_t* label = lv_obj_get_child(btn, 0);
                    if (label) {
                        char dir_text[64];
                        snprintf(dir_text, sizeof(dir_text), "[DIR] %s", browser->entries[i].name);
                        lv_label_set_text(label, dir_text);
                    }
                }
            }
            
            // Update visual selection
            update_visual_selection(browser);
            
            ESP_LOGI(TAG, "Updated file browser UI with %d entries", entry_count);
        } else {
            // Show "No files found..." message
            lv_obj_t* btn = lv_list_add_btn(browser->list, NULL, "No files found...");
            ui_theme_apply_button_style(btn, false);
            lv_obj_set_style_text_color(btn, UI_COLOR_TEXT, LV_PART_MAIN);
            lv_obj_set_style_text_font(btn, &lv_font_montserrat_18, LV_PART_MAIN);
            ESP_LOGI(TAG, "No files found in directory");
        }
        LV_UNLOCK();
    } else {
        ESP_LOGE(TAG, "Failed to parse browse response");
        // Show error message
        LV_LOCK();
        lv_obj_clean(browser->list);
        lv_obj_t* btn = lv_list_add_btn(browser->list, NULL, "Error loading files");
        ui_theme_apply_button_style(btn, false);
        lv_obj_set_style_text_color(btn, UI_COLOR_TEXT, LV_PART_MAIN);
        lv_obj_set_style_text_font(btn, &lv_font_montserrat_18, LV_PART_MAIN);
        LV_UNLOCK();
    }
}

// Helper function to update visual selection highlighting
static void update_visual_selection(wavex_file_browser_t* browser)
{
    if (!browser || !browser->list) return;
    
    LV_LOCK();
    // Get all buttons in the list
    uint32_t child_count = lv_obj_get_child_cnt(browser->list);
    
    for (uint32_t i = 0; i < child_count; i++) {
        lv_obj_t* btn = lv_obj_get_child(browser->list, i);
        if (btn) {
            if (i == browser->selected_index) {
                // Highlight selected item
                lv_obj_set_style_bg_color(btn, lv_color_make(0x33, 0x66, 0x99), LV_PART_MAIN);
                lv_obj_set_style_border_color(btn, lv_color_make(0x66, 0x99, 0xCC), LV_PART_MAIN);
                lv_obj_set_style_border_width(btn, 2, LV_PART_MAIN);
            } else {
                // Reset to normal style
                lv_obj_set_style_bg_color(btn, lv_color_make(0x22, 0x22, 0x22), LV_PART_MAIN);
                lv_obj_set_style_border_color(btn, lv_color_make(0x44, 0x44, 0x44), LV_PART_MAIN);
                lv_obj_set_style_border_width(btn, 1, LV_PART_MAIN);
            }
        }
    }
    LV_UNLOCK();
}
