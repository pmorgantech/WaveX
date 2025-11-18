/**
 * @file file_browser.cpp
 * @brief File Browser Component Implementation
 */

#include "file_browser.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "../styles/ui_theme.h"
#include "comm/i_comm_interface.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "inter_mcu.h"
#include "lvgl.h"
#include "spi_protocol/protocol.h"

#include <cstdio>
// Mark UI content changes so the main UI task triggers a refresh
#ifndef WAVEX_TEST_BUILD
#include "ui_task.h"
#else
#include "../../../tests/mocks/esp32_mocks.h"
#endif

using namespace WaveX::Protocol;

// LVGL includes for thread safety
#include "esp_lvgl_port.h"

// NOTE: LVGL locks are handled by UI task loop - all UI updates use deferred update pattern

static const char* TAG = "FILE_BROWSER";

// Browser instances are passed via user_data in callbacks - no global needed

// Forward declarations
static void file_list_event_cb(lv_event_t* e);
static bool refresh_file_list(wavex_file_browser_t* browser);
static bool parse_browse_response(const uint8_t* data,
                                  size_t length,
                                  wavex_file_entry_t* entries,
                                  uint32_t* count);
static bool parse_browse_response_with_pagination(const uint8_t* data,
                                                  size_t length,
                                                  wavex_file_entry_t* entries,
                                                  uint32_t* count,
                                                  uint32_t* total_files,
                                                  uint8_t* current_page_entries,
                                                  const char* current_path);
static bool send_browse_request(WaveX::Comm::ICommInterface* comm_interface,
                                const char* path,
                                uint8_t start_index = 0);
static void update_visual_selection(wavex_file_browser_t* browser);
static void browse_resp_callback(const uint8_t* data, size_t length, void* user_data);
static void update_file_browser_ui(wavex_file_browser_t* browser);

wavex_file_browser_t* wavex_file_browser_create(lv_obj_t* parent,
                                                const wavex_file_browser_config_t* config) {
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
    browser->first_visible_index = 0;
    browser->visible_count =
        8;  // Approximately 8 entries visible on screen (adjust based on screen size)

    // Initialize pagination state
    browser->total_files = 0;
    browser->current_page = 0;
    browser->entries_per_page =
        20;  // Daisy now sends 20 entries per page with flexible packet system
    browser->pagination_in_progress = false;
    browser->loaded_entries = 0;

    // Copy root path
    strncpy(browser->current_path,
            config->root_path ? config->root_path : "/",
            sizeof(browser->current_path) - 1);
    browser->current_path[sizeof(browser->current_path) - 1] = '\0';

    // Allocate entries array
    browser->entries =
        (wavex_file_entry_t*)malloc(config->max_entries * sizeof(wavex_file_entry_t));
    if (!browser->entries) {
        ESP_LOGE(TAG, "Failed to allocate entries array");
        free(browser);
        return NULL;
    }

    // Create main container (page creation callbacks already have LVGL context)
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
    // Note: Do not perform arithmetic with lv_pct(); set full height and offset below path label
    lv_obj_set_size(browser->list, lv_pct(100), lv_pct(100));
    lv_obj_align(
        browser->list, LV_ALIGN_TOP_LEFT, 0, 40);  // visually below the 40px-tall path label
    lv_obj_set_style_bg_color(browser->list, UI_COLOR_CONTENT, LV_PART_MAIN);
    lv_obj_set_style_border_width(browser->list, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(browser->list, UI_PADDING_SMALL, LV_PART_MAIN);

    // Add event callback for list
    lv_obj_add_event_cb(browser->list, file_list_event_cb, LV_EVENT_CLICKED, browser);

    // Register browse response callback with comm interface
    if (config->comm_interface) {
        config->comm_interface->setBrowseResponseListener(browse_resp_callback, browser);
    } else {
        ESP_LOGE(TAG, "No comm interface provided to file browser");
        return NULL;
    }

    // Refresh file list
    ESP_LOGI(TAG, "Calling refresh_file_list for path: %s", browser->current_path);
    if (!refresh_file_list(browser)) {
        ESP_LOGE(TAG, "Failed to refresh file list");
    }

    // Log all file entries for debugging
    for (uint32_t i = 0; i < browser->entry_count; i++) {
        ESP_LOGI(TAG,
                 "Entry %d: %s (%s) - %s",
                 i,
                 browser->entries[i].name,
                 browser->entries[i].is_directory ? "DIR" : "FILE",
                 browser->entries[i].path);
    }
    return browser;
}

void wavex_file_browser_destroy(wavex_file_browser_t* browser) {
    if (!browser)
        return;

    // Browser cleanup handled via user_data parameter in callbacks

    if (browser->entries) {
        free(browser->entries);
    }

    if (browser->container) {
        // Page destruction should be called from LVGL context
        lv_obj_del(browser->container);
    }

    free(browser);
    ESP_LOGI(TAG, "File browser destroyed");
}

bool wavex_file_browser_navigate_to(wavex_file_browser_t* browser, const char* path) {
    if (!browser || !path)
        return false;

    strncpy(browser->current_path, path, sizeof(browser->current_path) - 1);
    browser->current_path[sizeof(browser->current_path) - 1] = '\0';

    // Mark path update as pending (navigation may be called from non-LVGL context)
    browser->ui_update_pending = true;
    wavex_ui_mark_content_changed();

    // Reset scroll position and selection when navigating
    browser->first_visible_index = 0;
    browser->selected_index = 0;

    // Refresh file list
    bool success = refresh_file_list(browser);

    // Notify directory changed callback
    if (success && browser->dir_changed_cb) {
        browser->dir_changed_cb(browser->current_path, browser->user_data);
    }

    return success;
}

bool wavex_file_browser_navigate_up(wavex_file_browser_t* browser) {
    if (!browser)
        return false;

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

bool wavex_file_browser_refresh(wavex_file_browser_t* browser) {
    if (!browser)
        return false;

    return refresh_file_list(browser);
}

void wavex_file_browser_set_selection(wavex_file_browser_t* browser, uint32_t index) {
    if (!browser || index >= browser->entry_count)
        return;

    browser->selected_index = index;

    // Update viewport to ensure selected entry is visible
    if (browser->selected_index < browser->first_visible_index) {
        browser->first_visible_index = browser->selected_index;
    } else {
        uint32_t last_visible_index = browser->first_visible_index + browser->visible_count - 1;
        if (browser->selected_index > last_visible_index) {
            browser->first_visible_index = browser->selected_index - (browser->visible_count - 1);
            if (browser->first_visible_index > browser->selected_index) {
                browser->first_visible_index = 0;
            }
        }
    }

    ESP_LOGI(TAG, "Selected entry %d: %s", index, browser->entries[index].name);

    // Mark visual selection update as pending (selection may be changed from non-LVGL context)
    browser->ui_update_pending = true;
    wavex_ui_mark_content_changed();
}

const wavex_file_entry_t* wavex_file_browser_get_selected(wavex_file_browser_t* browser) {
    if (!browser || browser->selected_index >= browser->entry_count)
        return NULL;

    return &browser->entries[browser->selected_index];
}

uint32_t wavex_file_browser_get_selected_index(wavex_file_browser_t* browser) {
    if (!browser)
        return 0;

    return browser->selected_index;
}

// Navigate selection up with boundary checking and scrolling
bool wavex_file_browser_navigate_up_entry(wavex_file_browser_t* browser) {
    if (!browser || browser->entry_count == 0) {
        ESP_LOGD(TAG, "navigate_up: Invalid browser or no entries");
        return false;
    }

    // If already at first entry, do nothing (no wrap-around)
    if (browser->selected_index == 0) {
        ESP_LOGD(TAG, "navigate_up: Already at first entry (index 0)");
        return false;
    }

    uint32_t new_index = browser->selected_index - 1;
    browser->selected_index = new_index;
    ESP_LOGI(TAG, "navigate_up: moved from %u to %u", browser->selected_index + 1, new_index);

    // Update viewport if selection moved above visible area
    if (browser->selected_index < browser->first_visible_index) {
        browser->first_visible_index = browser->selected_index;
        ESP_LOGD(TAG, "navigate_up: scrolled viewport to %u", browser->first_visible_index);
    }

    // Notify callback about selection change
    if (browser->file_selected_index_cb && browser->entries) {
        browser->file_selected_index_cb(browser->selected_index,
                                        &browser->entries[browser->selected_index],
                                        browser->user_data);
    }

    // Update visual highlighting - refresh UI if viewport might have changed
    browser->ui_update_pending = true;
    wavex_ui_mark_content_changed();

    return true;
}

// Navigate selection down with boundary checking and scrolling
bool wavex_file_browser_navigate_down_entry(wavex_file_browser_t* browser) {
    if (!browser || browser->entry_count == 0) {
        ESP_LOGD(TAG, "navigate_down: Invalid browser or no entries");
        return false;
    }

    uint32_t last_index = browser->entry_count - 1;

    // If already at last entry, do nothing (no wrap-around)
    if (browser->selected_index >= last_index) {
        ESP_LOGD(TAG, "navigate_down: Already at last entry (index %u)", browser->selected_index);
        return false;
    }

    uint32_t new_index = browser->selected_index + 1;
    browser->selected_index = new_index;
    ESP_LOGI(TAG, "navigate_down: moved from %u to %u", browser->selected_index - 1, new_index);

    // Update viewport if selection moved below visible area
    uint32_t last_visible_index = browser->first_visible_index + browser->visible_count - 1;
    if (browser->selected_index > last_visible_index) {
        // Scroll down to show the selected entry
        browser->first_visible_index = browser->selected_index - (browser->visible_count - 1);
        // Ensure first_visible_index doesn't go negative (unsigned will wrap, so check bounds)
        if (browser->first_visible_index > browser->selected_index) {
            browser->first_visible_index = 0;
        }
        ESP_LOGD(TAG, "navigate_down: scrolled viewport to %u", browser->first_visible_index);
    }

    // Notify callback about selection change
    if (browser->file_selected_index_cb && browser->entries) {
        browser->file_selected_index_cb(browser->selected_index,
                                        &browser->entries[browser->selected_index],
                                        browser->user_data);
    }

    // Update visual highlighting - refresh UI if viewport might have changed
    browser->ui_update_pending = true;
    wavex_ui_mark_content_changed();

    return true;
}

void wavex_file_browser_set_file_selected_callback(wavex_file_browser_t* browser,
                                                   wavex_file_selected_cb_t callback,
                                                   void* user_data) {
    if (!browser)
        return;

    browser->file_selected_cb = callback;
    browser->user_data = user_data;
}

void wavex_file_browser_set_file_selected_index_callback(wavex_file_browser_t* browser,
                                                         wavex_file_selected_index_cb_t callback,
                                                         void* user_data) {
    if (!browser)
        return;

    browser->file_selected_index_cb = callback;
    browser->user_data = user_data;
}

void wavex_file_browser_set_directory_changed_callback(wavex_file_browser_t* browser,
                                                       wavex_directory_changed_cb_t callback,
                                                       void* user_data) {
    if (!browser)
        return;

    browser->dir_changed_cb = callback;
    browser->user_data = user_data;
}

const char* wavex_file_browser_get_current_path(wavex_file_browser_t* browser) {
    if (!browser)
        return NULL;

    return browser->current_path;
}

uint32_t wavex_file_browser_get_entry_count(wavex_file_browser_t* browser) {
    if (!browser)
        return 0;

    return browser->entry_count;
}

const wavex_file_entry_t* wavex_file_browser_get_entry(wavex_file_browser_t* browser,
                                                       uint32_t index) {
    if (!browser || index >= browser->entry_count)
        return NULL;

    return &browser->entries[index];
}

// File list event callback
static void file_list_event_cb(lv_event_t* e) {
    lv_event_code_t code = lv_event_get_code(e);
    wavex_file_browser_t* browser = (wavex_file_browser_t*)lv_event_get_user_data(e);

    if (code == LV_EVENT_CLICKED) {
        // lv_event_get_current_target returns the object the callback was attached to (the list)
        // lv_event_get_target returns the actual object that was clicked (the button)
        lv_obj_t* list = (lv_obj_t*)lv_event_get_current_target(e);
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
                    snprintf(new_path,
                             sizeof(new_path),
                             "%s/%s",
                             browser->current_path,
                             browser->entries[entry_index].name);
                }
                ESP_LOGD(TAG,
                         "Directory navigation: current='%s', name='%s' -> new_path='%s'",
                         browser->current_path,
                         browser->entries[entry_index].name,
                         new_path);
                wavex_file_browser_navigate_to(browser, new_path);
            } else {
                // File selected - notify callback with index
                ESP_LOGI(TAG,
                         "File selected: %s, index=%d",
                         browser->entries[entry_index].name,
                         entry_index);

                if (browser->file_selected_index_cb) {
                    browser->file_selected_index_cb(
                        entry_index, &browser->entries[entry_index], browser->user_data);
                } else {
                    ESP_LOGW(TAG, "No callback set for file selection");
                }
            }
        }
    }
}

// Refresh file list by requesting directory contents from Daisy
static bool refresh_file_list(wavex_file_browser_t* browser) {
    if (!browser)
        return false;

    ESP_LOGI(TAG, "refresh_file_list called for path: %s", browser->current_path);

    // Reset pagination state
    browser->total_files = 0;
    browser->current_page = 0;
    browser->pagination_in_progress = true;
    browser->loaded_entries = 0;
    browser->entry_count = 0;

    // Mark UI update as pending (will clear list when processed in UI task)
    browser->ui_update_pending = true;
    wavex_ui_mark_content_changed();

    // Send first browse request (page 0)
    if (!send_browse_request(browser->config.comm_interface, browser->current_path, 0)) {
        ESP_LOGE(TAG, "Failed to send browse request");
        browser->pagination_in_progress = false;
        // Mark error state for deferred UI update
        browser->entry_count = 0;
        browser->ui_update_pending = true;
        wavex_ui_mark_content_changed();
        return false;
    } else {
        ESP_LOGI(TAG, "Sent browse request for page 0, waiting for response...");
        return true;
    }
}

// Parse browse response from Daisy using payload format (not full packet)
static bool parse_browse_response(const uint8_t* data,
                                  size_t length,
                                  wavex_file_entry_t* entries,
                                  uint32_t* count) {
    if (!data || length < sizeof(BrowseRespHeader)) {
        ESP_LOGE(TAG,
                 "Browse response payload too short: %d bytes (need at least %d)",
                 (int)length,
                 (int)sizeof(BrowseRespHeader));
        *count = 0;
        return false;
    }

    // Parse payload directly: BrowseRespHeader + FileEntryWire entries
    const BrowseRespHeader* browse_header = (const BrowseRespHeader*)data;
    uint32_t total_count = browse_header->total_count;
    uint8_t n_entries = browse_header->n;

    ESP_LOGI(TAG,
             "Browse response: total_count=%lu, n_entries=%u",
             (unsigned long)total_count,
             n_entries);

    // Debug: Show raw payload data
    ESP_LOGI(TAG, "Raw payload (first 64 bytes):");
    for (int i = 0; i < 64 && i < (int)length; i++) {
        if (i % 16 == 0)
            ESP_LOGI(TAG, "");
        ESP_LOGI(TAG, "%02X ", data[i]);
    }
    ESP_LOGI(TAG, "");

    // Validate we have enough data for the header + entries
    size_t expected_size = sizeof(BrowseRespHeader) + (n_entries * sizeof(FileEntryWire));
    if (length < expected_size) {
        ESP_LOGE(TAG,
                 "Browse response payload too short: got %d bytes, expected %d",
                 (int)length,
                 (int)expected_size);
        *count = 0;
        return false;
    }

    // Parse file entries
    uint32_t parsed_count = 0;
    const FileEntryWire* wire_entries = (const FileEntryWire*)(data + sizeof(BrowseRespHeader));

    ESP_LOGI(TAG, "Starting file entry parsing: n_entries=%u, max_count=%u", n_entries, *count);

    for (uint8_t i = 0; i < n_entries && parsed_count < *count; i++) {
        const FileEntryWire* wire = &wire_entries[i];

        ESP_LOGI(TAG,
                 "Parsing entry %d: is_dir=%d, size=%lu, name='%.50s'",
                 i,
                 wire->is_dir,
                 (unsigned long)wire->size_bytes,
                 wire->name);

        // Copy to our file entry structure
        wavex_file_entry_t* entry = &entries[parsed_count++];
        entry->is_directory = wire->is_dir != 0;
        entry->size_bytes = wire->size_bytes;
        strncpy(entry->name, wire->name, sizeof(entry->name) - 1);
        entry->name[sizeof(entry->name) - 1] = '\0';

        // Create full path - for now just use the name, full path will be constructed when needed
        strncpy(entry->path, entry->name, sizeof(entry->path) - 1);
        entry->path[sizeof(entry->path) - 1] = '\0';

        ESP_LOGI(TAG,
                 "Parsed entry %d: '%s' (%s) - %lu bytes",
                 i,
                 entry->name,
                 entry->is_directory ? "DIR" : "FILE",
                 (unsigned long)entry->size_bytes);
    }

    *count = parsed_count;
    return true;
}

// Parse browse response with pagination information using payload format
static bool parse_browse_response_with_pagination(const uint8_t* data,
                                                  size_t length,
                                                  wavex_file_entry_t* entries,
                                                  uint32_t* count,
                                                  uint32_t* total_files,
                                                  uint8_t* current_page_entries,
                                                  const char* current_path) {
    if (!data || length < sizeof(BrowseRespHeader)) {
        ESP_LOGE(TAG,
                 "Browse response payload too short: %d bytes (need at least %d)",
                 (int)length,
                 (int)sizeof(BrowseRespHeader));
        *count = 0;
        *total_files = 0;
        *current_page_entries = 0;
        return false;
    }

    ESP_LOGI(TAG, "Parsing browse response payload: %d bytes", (int)length);

    // Parse payload directly: BrowseRespHeader + FileEntryWire entries
    const BrowseRespHeader* browse_header = (const BrowseRespHeader*)data;
    *total_files = browse_header->total_count;
    *current_page_entries = browse_header->n;

    ESP_LOGI(TAG,
             "Browse response: total_count=%lu, n_entries=%u",
             (unsigned long)*total_files,
             *current_page_entries);

    // Validate we have enough data for the header + entries
    size_t expected_size =
        sizeof(BrowseRespHeader) + (*current_page_entries * sizeof(FileEntryWire));
    if (length < expected_size) {
        ESP_LOGE(TAG,
                 "Browse response payload too short: got %d bytes, expected %d",
                 (int)length,
                 (int)expected_size);
        *count = 0;
        *total_files = 0;
        *current_page_entries = 0;
        return false;
    }

    // Parse file entries
    uint32_t parsed_count = 0;
    const FileEntryWire* wire_entries = (const FileEntryWire*)(data + sizeof(BrowseRespHeader));

    ESP_LOGI(TAG,
             "Starting file entry parsing: n_entries=%u, max_count=%u",
             *current_page_entries,
             *count);

    for (uint8_t i = 0; i < *current_page_entries && parsed_count < *count; i++) {
        const FileEntryWire* wire_entry = &wire_entries[i];
        wavex_file_entry_t* entry = &entries[parsed_count];

        entry->is_directory = wire_entry->is_dir != 0;
        entry->size_bytes = wire_entry->size_bytes;

        // Copy WAV metadata
        entry->sample_rate = wire_entry->sample_rate;
        entry->channels = wire_entry->channels;
        entry->bits_per_sample = wire_entry->bits_per_sample;
        entry->duration_ms = wire_entry->duration_ms;

        // Copy name with null termination
        strncpy(entry->name, wire_entry->name, sizeof(entry->name) - 1);
        entry->name[sizeof(entry->name) - 1] = '\0';

        // Strip leading slash if present (Daisy sometimes includes it)
        if (entry->name[0] == '/') {
            memmove(entry->name, entry->name + 1, strlen(entry->name));
        }

        ESP_LOGD(TAG,
                 "Entry %d: Raw name from Daisy: '%s', after slash strip: '%s'",
                 i,
                 wire_entry->name,
                 entry->name);

        // Build full path with bounds checking
        const char* path_base = current_path ? current_path : "/";

        ESP_LOGD(TAG, "Entry %d: current_path='%s', entry->name='%s'", i, path_base, entry->name);

        // Ensure we don't have double slashes - remove trailing slash from current_path if present
        const char* base_path = path_base;
        if (strlen(path_base) > 1 && path_base[strlen(path_base) - 1] == '/') {
            // Create a temporary string without trailing slash
            static char temp_path[96];
            strncpy(temp_path, path_base, sizeof(temp_path) - 1);
            temp_path[sizeof(temp_path) - 1] = '\0';
            temp_path[strlen(temp_path) - 1] = '\0';  // Remove trailing slash
            base_path = temp_path;
        }

        // Special case: if base_path is "/" and entry->name starts with "/", avoid double slash
        int path_len;
        if (strcmp(base_path, "/") == 0 && entry->name[0] == '/') {
            path_len = snprintf(entry->path, sizeof(entry->path), "%s", entry->name);
            ESP_LOGD(TAG,
                     "Path construction (root case): base='%s', name='%s' -> path='%s'",
                     base_path,
                     entry->name,
                     entry->path);
        } else {
            path_len = snprintf(entry->path, sizeof(entry->path), "%s/%s", base_path, entry->name);
            ESP_LOGD(TAG,
                     "Path construction (normal case): base='%s', name='%s' -> path='%s'",
                     base_path,
                     entry->name,
                     entry->path);
        }
        if (path_len >= (int)sizeof(entry->path)) {
            // Path was truncated, ensure null termination
            entry->path[sizeof(entry->path) - 1] = '\0';
            ESP_LOGW(TAG, "Path truncated for entry: %s", entry->name);
        }

        parsed_count++;

        ESP_LOGI(TAG,
                 "Parsed entry %d: '%s' (%s) - %lu bytes",
                 i,
                 entry->name,
                 entry->is_directory ? "DIR" : "FILE",
                 (unsigned long)entry->size_bytes);
    }

    *count = parsed_count;
    return true;
}

// Send browse request to Daisy via comm interface
static bool send_browse_request(WaveX::Comm::ICommInterface* comm_interface,
                                const char* path,
                                uint8_t start_index) {
    if (!comm_interface) {
        ESP_LOGE(TAG, "No comm interface available for browse request");
        return false;
    }

    esp_err_t result = comm_interface->sendBrowseRequest(path, start_index);
    if (result != ESP_OK) {
        ESP_LOGE(TAG, "Failed to send browse request: %d", result);
        return false;
    }
    ESP_LOGI(
        TAG,
        "=== INTERFACE MESSAGE: Successfully sent browse request for path: %s, start_index: %d ===",
        path,
        start_index);
    return true;
}

// Browse response callback function
static void browse_resp_callback(const uint8_t* data, size_t length, void* user_data) {
    wavex_file_browser_t* browser = (wavex_file_browser_t*)user_data;
    if (!browser || !data || length == 0) {
        ESP_LOGE(TAG, "Invalid browse response callback parameters");
        return;
    }

    ESP_LOGI(TAG, "Received browse response: %d bytes", (int)length);

    // Parse the browse response to get total count and current page entries
    // Allocate on heap instead of stack to prevent stack overflow in uart_link task
    // Each entry is ~152 bytes, so 20 entries = ~3040 bytes - too large for task stack
    wavex_file_entry_t* temp_entries = (wavex_file_entry_t*)malloc(20 * sizeof(wavex_file_entry_t));
    if (!temp_entries) {
        ESP_LOGE(TAG, "Failed to allocate memory for browse response parsing");
        browser->pagination_in_progress = false;
        return;
    }

    uint32_t temp_count = 20;
    uint32_t total_files = 0;
    uint8_t current_page_entries = 0;

    bool parse_success = parse_browse_response_with_pagination(data,
                                                               length,
                                                               temp_entries,
                                                               &temp_count,
                                                               &total_files,
                                                               &current_page_entries,
                                                               browser->current_path);

    if (!parse_success) {
        free(temp_entries);
        ESP_LOGE(TAG, "Failed to parse browse response");
        browser->pagination_in_progress = false;
        // Mark error state for deferred UI update (this callback runs from UART task, not LVGL
        // context)
        browser->entry_count = 0;
        browser->ui_update_pending = true;
        wavex_ui_mark_content_changed();
        return;
    }

    // Update browser state
    if (browser->current_page == 0) {
        // First page - initialize total count
        browser->total_files = total_files;
        ESP_LOGI(TAG, "Total files in directory: %d", total_files);
    }

    // Add current page entries to the browser's entry array
    // Sort ".." entries to the top if present (only on first page, and only if not at root)
    uint32_t start_index = browser->loaded_entries;
    uint32_t parent_dir_index = 0xFFFFFFFF;  // Track where ".." is found (using max value)

    // First pass: find ".." entry position if on first page and not at root
    if (browser->current_page == 0 && strcmp(browser->current_path, "/") != 0) {
        for (uint32_t i = 0; i < current_page_entries; i++) {
            if (strcmp(temp_entries[i].name, "..") == 0) {
                parent_dir_index = i;
                break;
            }
        }
    }

    // Second pass: add entries, putting ".." first if found on first page
    uint32_t write_index = start_index;

    // If ".." was found on first page and we're not at root, put it first
    if (parent_dir_index != 0xFFFFFFFF && browser->current_page == 0 &&
        strcmp(browser->current_path, "/") != 0 && write_index < browser->config.max_entries) {
        browser->entries[write_index] = temp_entries[parent_dir_index];
        browser->loaded_entries++;
        write_index++;
    }

    // Add all other entries (skip ".." if already added at the top)
    for (uint32_t i = 0; i < current_page_entries && write_index < browser->config.max_entries;
         i++) {
        if (parent_dir_index != 0xFFFFFFFF && i == parent_dir_index) {
            continue;  // Skip ".." if already added at the top
        }
        browser->entries[write_index] = temp_entries[i];
        browser->loaded_entries++;
        write_index++;
    }

    ESP_LOGI(TAG,
             "Loaded page %d: %d entries, total loaded: %d/%d",
             browser->current_page,
             current_page_entries,
             browser->loaded_entries,
             browser->total_files);

    // Free the temporary array now that we've copied the data
    free(temp_entries);

    // Check if we need to load more pages
    bool has_more_pages = (browser->loaded_entries < browser->total_files) &&
                          (browser->loaded_entries < browser->config.max_entries);

    // Update UI immediately after first page loads for better user experience
    if (browser->current_page == 0) {
        // First page loaded - mark UI update as pending
        browser->entry_count = browser->loaded_entries;

        // Reset scroll position for new directory
        browser->first_visible_index = 0;

        // Ensure selected_index is within bounds (default to first entry, or ".." if present)
        if (browser->selected_index >= browser->entry_count) {
            browser->selected_index = 0;
        }

        ESP_LOGI(TAG, "First page loaded: marking %d entries for UI update", browser->entry_count);

        // Set flag for UI task to process (thread-safe deferred update)
        browser->ui_update_pending = true;
        wavex_ui_mark_content_changed();

        // DEBUG: Direct refresh test - bypass normal update mechanism
        ESP_LOGI(TAG, "DEBUG: Adding direct refresh via lv_async_call");
        lv_async_call(
            [](void* data) {
                wavex_file_browser_t* b = (wavex_file_browser_t*)data;
                if (!b || !b->list) {
                ESP_LOGE(TAG, "DIRECT REFRESH: invalid browser or list");
                    return;
                }

                ESP_LOGI(TAG,
                         "DIRECT REFRESH: list valid=%d, entry_count=%d",
                         lv_obj_is_valid(b->list) ? 1 : 0,
                         b->entry_count);

                if (!lv_obj_is_valid(b->list)) {
                ESP_LOGE(TAG, "DIRECT REFRESH: list object is invalid!");
                    return;
                }

                // Clear existing items
                lv_obj_clean(b->list);

                // Update path label
                if (b->path_label && lv_obj_is_valid(b->path_label)) {
                    lv_label_set_text(b->path_label, b->current_path);
                }

                if (b->entry_count > 0 && b->entries) {
                    // Calculate which entries to display (scrolling viewport)
                    uint32_t start_index = b->first_visible_index;
                    uint32_t end_index = start_index + b->visible_count;
                    if (end_index > b->entry_count) {
                        end_index = b->entry_count;
                    }

                ESP_LOGI(TAG,
                             "DIRECT REFRESH: adding %d items (start=%d, end=%d)",
                             end_index - start_index,
                             start_index,
                             end_index);

                    // Create list items for visible entries
                    for (uint32_t i = start_index; i < end_index; i++) {
                        // Safety check for entry access
                        if (!b->entries[i].name[0]) {
                ESP_LOGW(TAG, "DIRECT REFRESH: Skipping empty entry at index %d", i);
                            continue;
                        }

                        lv_obj_t* btn = lv_list_add_btn(b->list, NULL, b->entries[i].name);
                        if (!btn) {
                ESP_LOGE(
                                TAG, "DIRECT REFRESH: Failed to create button for entry %d", i);
                            continue;
                        }

                        // Apply styling
                        ui_theme_apply_button_style(btn, true);
                        lv_obj_set_style_text_color(btn, UI_COLOR_TEXT, LV_PART_MAIN);
                        lv_obj_set_style_text_font(btn, UI_FONT_TITLE, LV_PART_MAIN);

                        // Add directory indicator
                        if (b->entries[i].is_directory) {
                            lv_obj_t* label = lv_obj_get_child(btn, 0);
                            if (label) {
                                char dir_text[64];
                                snprintf(
                                    dir_text, sizeof(dir_text), "[DIR] %s", b->entries[i].name);
                                lv_label_set_text(label, dir_text);
                            }
                        }

                        // Store entry index for selection
                        lv_obj_set_user_data(btn, (void*)(uintptr_t)i);
                    }

                ESP_LOGI(TAG, "DIRECT REFRESH: successfully added items to list");
                } else {
                ESP_LOGI(TAG, "DIRECT REFRESH: no entries to display");
                    // Show "No files found..." message
                    lv_obj_t* btn = lv_list_add_btn(b->list, NULL, "No files found...");
                    ui_theme_apply_button_style(btn, false);
                    lv_obj_set_style_text_color(btn, UI_COLOR_TEXT, LV_PART_MAIN);
                    lv_obj_set_style_text_font(btn, UI_FONT_TITLE, LV_PART_MAIN);
                }

                ESP_LOGI(TAG, "DIRECT REFRESH: completed UI update");
            },
            browser);

        // Notify directory changed callback
        if (browser->dir_changed_cb) {
            browser->dir_changed_cb(browser->current_path, browser->user_data);
        }
    }

    if (has_more_pages) {
        // Request next page
        browser->current_page++;
        uint8_t next_start_index = browser->current_page * browser->entries_per_page;

        ESP_LOGI(TAG,
                 "Requesting next page: current_page=%d, entries_per_page=%d, start_index=%d",
                 browser->current_page,
                 browser->entries_per_page,
                 next_start_index);
        if (!send_browse_request(browser->config.comm_interface, browser->current_path, next_start_index)) {
            ESP_LOGE(TAG, "Failed to request next page");
            browser->pagination_in_progress = false;
        }
        // Continue loading additional pages in background
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

        // Mark UI update as pending (only if not first page)
        if (browser->current_page > 0) {
            browser->ui_update_pending = true;
            wavex_ui_mark_content_changed();
        }

        // Notify directory changed callback (only if not first page)
        if (browser->current_page > 0 && browser->dir_changed_cb) {
            browser->dir_changed_cb(browser->current_path, browser->user_data);
        }
    }
}

// Thread-safe function to process pending UI updates (called from UI task with LVGL lock)
void wavex_file_browser_process_pending_updates(wavex_file_browser_t* browser) {
    if (!browser) {
        ESP_LOGE(TAG, "process_pending_updates: browser is NULL!");
        return;
    }

    ESP_LOGD(TAG,
             "process_pending_updates: ui_update_pending=%d, entry_count=%d",
             browser->ui_update_pending ? 1 : 0,
             browser->entry_count);

    if (!browser->ui_update_pending) {
        return;  // No update needed
    }

    ESP_LOGI(
        TAG, "Processing pending UI update for file browser with %d entries", browser->entry_count);

    // Clear flag first
    browser->ui_update_pending = false;

    // Update UI (caller must hold LVGL lock - this is called from UI task loop)
    update_file_browser_ui(browser);

    ESP_LOGI(TAG, "UI update complete");
}

// Helper function to update the file browser UI
// NOTE: Must be called with LVGL lock already held (from UI task loop)
static void update_file_browser_ui(wavex_file_browser_t* browser) {
    if (!browser || !browser->list)
        return;

    // LVGL lock must be held by caller (UI task loop)
    // Update path label if needed
    if (browser->path_label) {
        lv_label_set_text(browser->path_label, browser->current_path);
    }

    lv_obj_clean(browser->list);

    if (browser->entry_count > 0 && browser->entries) {
        // Ensure first_visible_index is valid
        if (browser->first_visible_index >= browser->entry_count) {
            browser->first_visible_index = 0;
        }

        // Calculate which entries to display (scrolling viewport)
        uint32_t start_index = browser->first_visible_index;
        uint32_t end_index = start_index + browser->visible_count;
        if (end_index > browser->entry_count) {
            end_index = browser->entry_count;
        }

        // Create list items only for visible entries
        for (uint32_t i = start_index; i < end_index; i++) {
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

            // Store the actual entry index in user data for selection highlighting
            lv_obj_set_user_data(btn, (void*)(uintptr_t)i);
        }

        // Update visual selection (maps selected_index to visible button)
        update_visual_selection(browser);

        ESP_LOGI(TAG, "Updated file browser UI with %d entries", browser->entry_count);
    } else if (browser->entry_count == 0 && browser->pagination_in_progress == false) {
        // Show "No files found..." message (pagination complete but no entries)
        lv_obj_t* btn = lv_list_add_btn(browser->list, NULL, "No files found...");
        ui_theme_apply_button_style(btn, false);
        lv_obj_set_style_text_color(btn, UI_COLOR_TEXT, LV_PART_MAIN);
        lv_obj_set_style_text_font(btn, &lv_font_montserrat_18, LV_PART_MAIN);
        ESP_LOGI(TAG, "No files found in directory");
    } else {
        // Error state - show error message (entry_count is 0 but pagination not in progress =
        // error)
        lv_obj_t* btn = lv_list_add_btn(browser->list, NULL, "Error loading files");
        ui_theme_apply_button_style(btn, false);
        lv_obj_set_style_text_color(btn, UI_COLOR_TEXT, LV_PART_MAIN);
        lv_obj_set_style_text_font(btn, &lv_font_montserrat_18, LV_PART_MAIN);
        ESP_LOGI(TAG, "Showing error message in file browser");
    }

    // Mark content as changed to trigger refresh in UI task
    wavex_ui_mark_content_changed();
}

// Helper function to update visual selection highlighting
// NOTE: Must be called with LVGL lock already held
static void update_visual_selection(wavex_file_browser_t* browser) {
    if (!browser || !browser->list)
        return;

    // LVGL lock must be held by caller
    // Get all buttons in the list (these are only the visible ones)
    uint32_t child_count = lv_obj_get_child_cnt(browser->list);

    // Ensure selected_index is within bounds of all entries
    if (browser->selected_index >= browser->entry_count) {
        browser->selected_index = 0;  // Reset to first item if out of bounds
    }

    // Highlight the selected entry if it's visible
    for (uint32_t i = 0; i < child_count; i++) {
        lv_obj_t* btn = lv_obj_get_child(browser->list, i);
        if (btn) {
            // Get the actual entry index from user data
            uint32_t entry_index = (uint32_t)(uintptr_t)lv_obj_get_user_data(btn);

            if (entry_index == browser->selected_index) {
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
    // Mark content as changed to trigger refresh (lock held by caller)
    wavex_ui_mark_content_changed();
}
