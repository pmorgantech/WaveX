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
static bool parse_browse_response_with_pagination(const uint8_t* data, size_t length, wavex_file_entry_t* entries, uint32_t* count, uint32_t* total_files, uint8_t* current_page_entries);
static bool send_browse_request(const char* path, uint8_t start_index = 0);
static void update_visual_selection(wavex_file_browser_t* browser);
static void browse_resp_callback(const uint8_t* data, size_t length, void* user_data);

wavex_file_browser_t* wavex_file_browser_create(lv_obj_t* parent, const wavex_file_browser_config_t* config)
{
    if (!parent || !config) {
        ESP_LOGE(TAG, "Invalid parameters");
        return NULL;
    }
    
    wavex_file_browser_t* browser = (wavex_file_browser_t*)malloc(sizeof(wavex_file_browser_t));
    if (!browser) {
        ESP_LOGE(TAG, "Failed to allocate file browser structure");
        return NULL;
    }

    // Initialize structure
    memset(browser, 0, sizeof(wavex_file_browser_t));
    browser->config = *config;
    browser->selected_index = 0;
    
    // Initialize pagination state
    browser->total_files = 0;
    browser->current_page = 0;
    browser->entries_per_page = 4;  // Daisy sends 4 entries per page
    browser->pagination_in_progress = false;
    browser->loaded_entries = 0;
    
    // Copy root path
    strncpy(browser->current_path, config->root_path ? config->root_path : "/", sizeof(browser->current_path) - 1);
    browser->current_path[sizeof(browser->current_path) - 1] = '\0';
    
    // Allocate entries array
    browser->entries = (wavex_file_entry_t*)malloc(config->max_entries * sizeof(wavex_file_entry_t));
    if (!browser->entries) {
        ESP_LOGE(TAG, "Failed to allocate entries array");
        free(browser);
        return NULL;
    }
    
    // Create main container
    LV_LOCK();
    browser->container = lv_obj_create(parent);
    lv_obj_set_size(browser->container, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(browser->container, UI_COLOR_CONTENT, LV_PART_MAIN);
    lv_obj_set_style_border_width(browser->container, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(browser->container, 0, LV_PART_MAIN);
    lv_obj_align(browser->container, LV_ALIGN_TOP_LEFT, 0, 0);
    
    // Create path label
    browser->path_label = lv_label_create(browser->container);
    lv_label_set_text(browser->path_label, browser->current_path);
    ui_theme_apply_label_style(browser->path_label, false);
    lv_obj_align(browser->path_label, LV_ALIGN_TOP_LEFT, UI_PADDING_MEDIUM, UI_PADDING_MEDIUM);
    
    // Create file list
    browser->list = lv_list_create(browser->container);
    lv_obj_set_size(browser->list, lv_pct(100), lv_pct(100) - 40); // Leave space for path label
    lv_obj_align(browser->list, LV_ALIGN_TOP_LEFT, 0, 40);
    lv_obj_set_style_bg_color(browser->list, UI_COLOR_CONTENT, LV_PART_MAIN);
    lv_obj_set_style_border_width(browser->list, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(browser->list, UI_PADDING_SMALL, LV_PART_MAIN);
    
    // Add event callback for list
    lv_obj_add_event_cb(browser->list, file_list_event_cb, LV_EVENT_CLICKED, browser);
    LV_UNLOCK();
    
    // Set global reference and register callback
    g_current_browser = browser;

    // Register browse response callback with inter-MCU system
    extern void inter_mcu_set_browse_resp_listener(wavex_browse_resp_cb_t cb, void* user_data);
    inter_mcu_set_browse_resp_listener(browse_resp_callback, browser);
    
    // Refresh file list
    ESP_LOGI(TAG, "Calling refresh_file_list for path: %s", browser->current_path);
    if (!refresh_file_list(browser)) {
        ESP_LOGE(TAG, "Failed to refresh file list");
    }
    
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
    
    // Check if we're already at root directory
    if (strcmp(browser->current_path, "/") == 0) {
        ESP_LOGI(TAG, "Already at root directory, cannot navigate up");
        return false;
    }
    
    // Construct parent path without modifying current_path
    char parent_path[96];
    strncpy(parent_path, browser->current_path, sizeof(parent_path) - 1);
    parent_path[sizeof(parent_path) - 1] = '\0';
    
    // Find last directory separator
    char* last_slash = strrchr(parent_path, '/');
    if (last_slash && last_slash != parent_path) {
        *last_slash = '\0';
    } else {
        strcpy(parent_path, "/");
    }
    
    ESP_LOGI(TAG, "Navigating up: '%s' -> '%s'", browser->current_path, parent_path);
    
    return wavex_file_browser_navigate_to(browser, parent_path);
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

void wavex_file_browser_set_file_selected_index_callback(wavex_file_browser_t* browser, 
                                                         wavex_file_selected_index_cb_t callback, 
                                                         void* user_data)
{
    if (!browser) return;
    
    browser->file_selected_index_cb = callback;
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
        uint32_t ui_index = 0;
        lv_obj_t* child = lv_obj_get_child(list, 0);
        while (child && child != btn) {
            child = lv_obj_get_child(list, ui_index + 1);
            ui_index++;
        }
        
        // Handle ".." entries that come from Daisy
        uint32_t entry_index = ui_index;
        
        if (entry_index < browser->entry_count) {
            const wavex_file_entry_t* entry = &browser->entries[entry_index];
            if (strcmp(entry->name, "..") == 0) {
                // ".." entry clicked - navigate up
                ESP_LOGI(TAG, "Parent directory entry clicked");
                wavex_file_browser_navigate_up(browser);
                return;
            }
        }
        
        if (entry_index < browser->entry_count) {
            wavex_file_browser_set_selection(browser, entry_index);
            
            // If it's a directory, navigate into it
            if (browser->entries[entry_index].is_directory) {
                char new_path[256];
                // Avoid double slashes when constructing directory path
                if (strcmp(browser->current_path, "/") == 0) {
                    snprintf(new_path, sizeof(new_path), "/%s", browser->entries[entry_index].name);
                } else {
                    snprintf(new_path, sizeof(new_path), "%s/%s", browser->current_path, browser->entries[entry_index].name);
                }
                ESP_LOGD(TAG, "Directory navigation: current='%s', name='%s' -> new_path='%s'", browser->current_path, browser->entries[entry_index].name, new_path);
                wavex_file_browser_navigate_to(browser, new_path);
            } else {
                // File selected - notify callback with index
                ESP_LOGI(TAG, "File selected: %s, index=%d", browser->entries[entry_index].name, entry_index);
                
                if (browser->file_selected_index_cb) {
                    browser->file_selected_index_cb(entry_index, &browser->entries[entry_index], browser->user_data);
                } else {
                    ESP_LOGW(TAG, "No callback set for file selection");
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
    
    // Reset pagination state
    browser->total_files = 0;
    browser->current_page = 0;
    browser->pagination_in_progress = true;
    browser->loaded_entries = 0;
    browser->entry_count = 0;
    
    // Clear existing list items
    LV_LOCK();
    lv_obj_clean(browser->list);
    LV_UNLOCK();
    
    // Send first browse request (page 0)
    if (!send_browse_request(browser->current_path, 0)) {
        ESP_LOGE(TAG, "Failed to send browse request");
        browser->pagination_in_progress = false;
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
        ESP_LOGI(TAG, "Sent browse request for page 0, waiting for response...");
        return true;
    }
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

    // ESP_LOGI(TAG, "Starting file entry parsing: n_entries=%u, max_count=%u", n_entries, *count);
    // ESP_LOGI(TAG, "Wire entries pointer: %p, payload offset: %d", wire_entries, (int)(payload - data + sizeof(WaveX::Protocol::BrowseRespHeader)));

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

// Parse browse response with pagination information
static bool parse_browse_response_with_pagination(const uint8_t* data, size_t length, wavex_file_entry_t* entries, uint32_t* count, uint32_t* total_files, uint8_t* current_page_entries)
{
    if (!data || length < sizeof(WaveX::Protocol::PacketHeader) + sizeof(WaveX::Protocol::BrowseRespHeader)) {
        ESP_LOGE(TAG, "Browse response too short: %d bytes", (int)length);
        *count = 0;
        *total_files = 0;
        *current_page_entries = 0;
        return false;
    }

    // Parse as PacketHeader + payload format (not Packet structure)
    const WaveX::Protocol::PacketHeader* header = (const WaveX::Protocol::PacketHeader*)data;
    if (header->type != WaveX::Protocol::MSG_BROWSE_RESP) {
        ESP_LOGE(TAG, "Wrong message type: expected %d, got %d", WaveX::Protocol::MSG_BROWSE_RESP, header->type);
        *count = 0;
        *total_files = 0;
        *current_page_entries = 0;
        return false;
    }

    const uint8_t* payload = data + sizeof(WaveX::Protocol::PacketHeader);
    const WaveX::Protocol::BrowseRespHeader* browse_header = (const WaveX::Protocol::BrowseRespHeader*)payload;
    *total_files = browse_header->total_count;
    *current_page_entries = browse_header->n;

    ESP_LOGI(TAG, "Browse response: total_count=%lu, n_entries=%u", (unsigned long)*total_files, *current_page_entries);
    
    // Parse file entries
    uint32_t parsed_count = 0;
    const WaveX::Protocol::FileEntryWire* wire_entries = (const WaveX::Protocol::FileEntryWire*)(payload + sizeof(WaveX::Protocol::BrowseRespHeader));

    ESP_LOGI(TAG, "Starting file entry parsing: n_entries=%u, max_count=%u", *current_page_entries, *count);

    for (uint8_t i = 0; i < *current_page_entries && parsed_count < *count; i++) {
        const WaveX::Protocol::FileEntryWire* wire_entry = &wire_entries[i];
        wavex_file_entry_t* entry = &entries[parsed_count];
        
        entry->is_directory = wire_entry->is_dir != 0;
        entry->size_bytes = wire_entry->size_bytes;
        
        // Copy name with null termination
        strncpy(entry->name, wire_entry->name, sizeof(entry->name) - 1);
        entry->name[sizeof(entry->name) - 1] = '\0';
        
        // Strip leading slash if present (Daisy sometimes includes it)
        if (entry->name[0] == '/') {
            memmove(entry->name, entry->name + 1, strlen(entry->name));
        }
        
        ESP_LOGD(TAG, "Entry %d: Raw name from Daisy: '%s', after slash strip: '%s'", i, wire_entry->name, entry->name);
        
        // Build full path with bounds checking
        const char* current_path = g_current_browser ? g_current_browser->current_path : "/";
        
        ESP_LOGD(TAG, "Entry %d: current_path='%s', entry->name='%s'", i, current_path, entry->name);
        
        // Ensure we don't have double slashes - remove trailing slash from current_path if present
        const char* base_path = current_path;
        if (strlen(current_path) > 1 && current_path[strlen(current_path) - 1] == '/') {
            // Create a temporary string without trailing slash
            static char temp_path[96];
            strncpy(temp_path, current_path, sizeof(temp_path) - 1);
            temp_path[sizeof(temp_path) - 1] = '\0';
            temp_path[strlen(temp_path) - 1] = '\0'; // Remove trailing slash
            base_path = temp_path;
        }
        
        // Special case: if base_path is "/" and entry->name starts with "/", avoid double slash
        int path_len;
        if (strcmp(base_path, "/") == 0 && entry->name[0] == '/') {
            path_len = snprintf(entry->path, sizeof(entry->path), "%s", entry->name);
            ESP_LOGD(TAG, "Path construction (root case): base='%s', name='%s' -> path='%s'", base_path, entry->name, entry->path);
        } else {
            path_len = snprintf(entry->path, sizeof(entry->path), "%s/%s", base_path, entry->name);
            ESP_LOGD(TAG, "Path construction (normal case): base='%s', name='%s' -> path='%s'", base_path, entry->name, entry->path);
        }
        if (path_len >= (int)sizeof(entry->path)) {
            // Path was truncated, ensure null termination
            entry->path[sizeof(entry->path) - 1] = '\0';
            ESP_LOGW(TAG, "Path truncated for entry: %s", entry->name);
        }
        
        parsed_count++;
        
        ESP_LOGI(TAG, "Parsed entry %d: '%s' (%s) - %lu bytes", 
                 i, entry->name, entry->is_directory ? "DIR" : "FILE", (unsigned long)entry->size_bytes);
    }

    *count = parsed_count;
    return true;
}

// Send browse request to Daisy
static bool send_browse_request(const char* path, uint8_t start_index)
{
    esp_err_t result = inter_mcu_send_browse_req(path, start_index);
    if (result != ESP_OK) {
        ESP_LOGE(TAG, "Failed to send browse request: %d", result);
        return false;
    }
    ESP_LOGI(TAG, "=== SPI OPERATION: Successfully sent browse request for path: %s, start_index: %d ===", path, start_index);
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
    
    // // Log raw data for debugging (first 64 bytes)
    // ESP_LOGI(TAG, "Raw data (first 64 bytes):");
    // for (int i = 0; i < (int)length && i < 64; i++) {
    //     if (i % 16 == 0) {
    //         ESP_LOGI(TAG, "  %04X: ", i);
    //     }
    //     ESP_LOGI(TAG, "%02X ", data[i]);
    //     if (i % 16 == 15) {
    //         ESP_LOGI(TAG, "");
    //     }
    // }
    // if (length % 16 != 0) {
    //     ESP_LOGI(TAG, "");
    // }
    
    // Parse the browse response to get total count and current page entries
    wavex_file_entry_t temp_entries[4];  // Temporary array for current page
    uint32_t temp_count = 4;
    uint32_t total_files = 0;
    uint8_t current_page_entries = 0;
    
    if (!parse_browse_response_with_pagination(data, length, temp_entries, &temp_count, &total_files, &current_page_entries)) {
        ESP_LOGE(TAG, "Failed to parse browse response");
        browser->pagination_in_progress = false;
        // Show error message
        LV_LOCK();
        lv_obj_clean(browser->list);
        lv_obj_t* btn = lv_list_add_btn(browser->list, NULL, "Error loading files");
        ui_theme_apply_button_style(btn, false);
        lv_obj_set_style_text_color(btn, UI_COLOR_TEXT, LV_PART_MAIN);
        lv_obj_set_style_text_font(btn, &lv_font_montserrat_18, LV_PART_MAIN);
        LV_UNLOCK();
        return;
    }
    
    // Update browser state
    if (browser->current_page == 0) {
        // First page - initialize total count
        browser->total_files = total_files;
        ESP_LOGI(TAG, "Total files in directory: %d", total_files);
    }
    
    // Add current page entries to the browser's entry array
    uint32_t start_index = browser->loaded_entries;
    for (uint32_t i = 0; i < current_page_entries && (start_index + i) < browser->config.max_entries; i++) {
        browser->entries[start_index + i] = temp_entries[i];
        browser->loaded_entries++;
    }
    
    ESP_LOGI(TAG, "Loaded page %d: %d entries, total loaded: %d/%d", 
             browser->current_page, current_page_entries, browser->loaded_entries, browser->total_files);
    
    // Check if we need to load more pages
    bool has_more_pages = (browser->loaded_entries < browser->total_files) && 
                         (browser->loaded_entries < browser->config.max_entries);
    
    if (has_more_pages) {
        // Request next page
        browser->current_page++;
        uint8_t next_start_index = browser->current_page * browser->entries_per_page;
        
        ESP_LOGI(TAG, "Requesting next page: start_index=%d", next_start_index);
        if (!send_browse_request(browser->current_path, next_start_index)) {
            ESP_LOGE(TAG, "Failed to request next page");
            browser->pagination_in_progress = false;
        }
        // Don't update UI yet - wait for all pages to load
        return;
    } else {
        // All pages loaded (or reached max entries)
        browser->pagination_in_progress = false;
        browser->entry_count = browser->loaded_entries;
        
        // Ensure selected_index is within bounds
        if (browser->selected_index >= browser->entry_count) {
            browser->selected_index = 0;
        }
        
        ESP_LOGI(TAG, "Pagination complete: loaded %d entries", browser->entry_count);
        
        // Update UI with all loaded entries
        LV_LOCK();
        lv_obj_clean(browser->list);
        
        // Note: ".." entries are now provided by Daisy backend, no need to create them manually
        bool added_parent_entry = false;
        
        if (browser->entry_count > 0 && browser->entries) {
            // Create list items for all loaded entries
            for (uint32_t i = 0; i < browser->entry_count; i++) {
                // Safety check for entry access
                if (!browser->entries[i].name[0]) {
                    ESP_LOGW(TAG, "Skipping empty entry at index %d", i);
                    continue;
                }
                
                lv_obj_t* btn = lv_list_add_btn(browser->list, NULL, browser->entries[i].name);
                if (!btn) {
                    ESP_LOGE(TAG, "Failed to create button for entry %d", i);
                    continue;
                }
                
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
            
            ESP_LOGI(TAG, "Updated file browser UI with %d entries", browser->entry_count);
        } else {
            // Show "No files found..." message
            lv_obj_t* btn = lv_list_add_btn(browser->list, NULL, "No files found...");
            ui_theme_apply_button_style(btn, false);
            lv_obj_set_style_text_color(btn, UI_COLOR_TEXT, LV_PART_MAIN);
            lv_obj_set_style_text_font(btn, &lv_font_montserrat_18, LV_PART_MAIN);
            ESP_LOGI(TAG, "No files found in directory");
        }
        
        LV_UNLOCK();
        
        // Notify directory changed callback
        if (browser->dir_changed_cb) {
            browser->dir_changed_cb(browser->current_path, browser->user_data);
        }
    }
}

// Helper function to update visual selection highlighting
static void update_visual_selection(wavex_file_browser_t* browser)
{
    if (!browser || !browser->list) return;
    
    LV_LOCK();
    // Get all buttons in the list
    uint32_t child_count = lv_obj_get_child_cnt(browser->list);
    
    // Ensure selected_index is within bounds
    if (browser->selected_index >= child_count) {
        browser->selected_index = 0; // Reset to first item if out of bounds
    }
    
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
