// WaveX UI Sample Browser Implementation
#include "ui/ui_sample_browser.h"
#include "../styles/ui_theme.h"
#include <esp_log.h>
#include <cstring>
#include <cstdio>
#include "../../../main/inter_mcu.h"
#include "../../../main/ui_task.h"
#include "esp_lvgl_port.h"

static const char* TAG = "UI_SAMPLE_BROWSER";

namespace wavex_ui {

// Static state preservation
char UISampleBrowser::s_last_directory_path_[96] = "/";
uint32_t UISampleBrowser::s_last_selected_index_ = 0;
UISampleBrowser* UISampleBrowser::s_active_instance_ = nullptr;

UISampleBrowser::UISampleBrowser() {
}

UISampleBrowser::~UISampleBrowser() {
    // Cleanup is done in onExit()
}

void UISampleBrowser::onEnter(lv_obj_t* parent) {
    // NOTE: onEnter is called from UINavigator::push/pop which already holds LVGL lock
    // No need to acquire lock here
    
    // Set as active instance for callbacks
    s_active_instance_ = this;
    
    root_ = lv_obj_create(parent);
    lv_obj_set_size(root_, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(root_, UI_COLOR_CONTENT, LV_PART_MAIN);
    lv_obj_set_style_border_width(root_, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(root_, 0, LV_PART_MAIN);
    lv_obj_align(root_, LV_ALIGN_TOP_LEFT, 0, 0);
    
    // Create file browser container (left side, 70% width)
    browser_container_ = lv_obj_create(root_);
    lv_obj_set_size(browser_container_, lv_pct(70), lv_pct(100));
    lv_obj_align(browser_container_, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_style_bg_color(browser_container_, UI_COLOR_CONTENT, LV_PART_MAIN);
    lv_obj_set_style_border_width(browser_container_, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(browser_container_, 0, LV_PART_MAIN);
    
    // Create info panel (right side, 30% width)
    info_panel_ = lv_obj_create(root_);
    lv_obj_set_size(info_panel_, lv_pct(30), lv_pct(100));
    ui_theme_apply_container_style(info_panel_, true);
    lv_obj_align(info_panel_, LV_ALIGN_TOP_RIGHT, -UI_PADDING_MEDIUM, UI_PADDING_MEDIUM);
    
    // Create status label in info panel
    status_label_ = lv_label_create(info_panel_);
    lv_label_set_text(status_label_, "Ready");
    ui_theme_apply_label_style(status_label_, false);
    lv_obj_align(status_label_, LV_ALIGN_TOP_LEFT, UI_PADDING_MEDIUM, UI_PADDING_MEDIUM);
    
    // Create metadata label in info panel
    metadata_label_ = lv_label_create(info_panel_);
    lv_label_set_text(metadata_label_, "Select a file to view metadata");
    ui_theme_apply_label_style(metadata_label_, false);
    lv_obj_align(metadata_label_, LV_ALIGN_TOP_LEFT, UI_PADDING_MEDIUM, 60);
    lv_obj_set_style_text_align(metadata_label_, LV_TEXT_ALIGN_LEFT, LV_PART_MAIN);
    
    // Configure file browser - restore last directory path if available
    wavex_file_browser_config_t browser_config = {
        .root_path = s_last_directory_path_,
        .file_extension = ".wav",
        .max_entries = 50,
        .show_hidden = false
    };
    
    ESP_LOGI(TAG, "Creating file browser with root_path: %s", browser_config.root_path);
    
    file_browser_ = wavex_file_browser_create(browser_container_, &browser_config);
    if (!file_browser_) {
        ESP_LOGE(TAG, "Failed to create file browser");
        return;
    }
    
    // Set callbacks
    wavex_file_browser_set_file_selected_callback(file_browser_, file_selected_callback, this);
    wavex_file_browser_set_file_selected_index_callback(file_browser_, file_selected_index_callback, this);
    wavex_file_browser_set_directory_changed_callback(file_browser_, directory_changed_callback, this);
    
    // Register sample status callback with inter-MCU system
    inter_mcu_set_sample_status_listener(sample_status_callback, this);
    
    // Initialize state
    is_playing_ = false;
    selected_file_index_ = 0;
    memset(selected_file_path_, 0, sizeof(selected_file_path_));
    
    ESP_LOGI(TAG, "Sample Browser page created");
}

void UISampleBrowser::onExit() {
    // NOTE: onExit is called from UINavigator::push/pop which already holds LVGL lock
    // No need to acquire lock here
    
    // NOTE: Do NOT stop playback when navigating away - allow playback to continue
    // This allows users to browse other pages while audio plays in the background
    
    // Unregister callbacks
    inter_mcu_set_sample_status_listener(nullptr, nullptr);
    
    // Preserve state before destroying
    if (file_browser_) {
        const char* current_path = wavex_file_browser_get_current_path(file_browser_);
        if (current_path) {
            strncpy(s_last_directory_path_, current_path, sizeof(s_last_directory_path_) - 1);
            s_last_directory_path_[sizeof(s_last_directory_path_) - 1] = '\0';
        }
        s_last_selected_index_ = wavex_file_browser_get_selected_index(file_browser_);
        
        wavex_file_browser_destroy(file_browser_);
        file_browser_ = nullptr;
    }
    
    // Clear active instance
    if (s_active_instance_ == this) {
        s_active_instance_ = nullptr;
    }
    
    // Clean up UI objects
    if (root_) {
        lv_obj_del(root_);
        root_ = nullptr;
        browser_container_ = nullptr;
        info_panel_ = nullptr;
        status_label_ = nullptr;
        metadata_label_ = nullptr;
    }
    
    ESP_LOGI(TAG, "Sample Browser page destroyed");
}

void UISampleBrowser::onInput(const InputEvent& evt) {
    // NOTE: onInput is called from UI task (not LVGL context), so we need locks for LVGL operations
    
    switch (evt.type) {
        case InputType::EncoderUp:
        case InputType::EncoderDown: {
            if (!file_browser_) break;
            
            ESP_LOGI(TAG, "Received %s event for file browser scrolling (delta=%d)", 
                     (evt.type == InputType::EncoderUp) ? "EncoderUp" : "EncoderDown", evt.delta);
            
            uint32_t current_index = wavex_file_browser_get_selected_index(file_browser_);
            uint32_t entry_count = wavex_file_browser_get_entry_count(file_browser_);
            
            if (entry_count == 0) {
                ESP_LOGW(TAG, "No entries in file browser");
                break;
            }
            
            // Handle each step of delta separately for responsive scrolling
            int steps = (evt.delta > 0) ? evt.delta : -evt.delta;
            bool result = false;
            
            for (int step = 0; step < steps && step < 10; step++) {
                if (evt.type == InputType::EncoderUp) {
                    result = wavex_file_browser_navigate_up_entry(file_browser_);
                } else {
                    result = wavex_file_browser_navigate_down_entry(file_browser_);
                }
                if (!result) {
                    ESP_LOGD(TAG, "Navigation reached boundary at step %d/%d", step, steps);
                    break;
                }
            }
            
            uint32_t new_index = wavex_file_browser_get_selected_index(file_browser_);
            ESP_LOGI(TAG, "After nav: new_index=%u, moved=%d steps", 
                     new_index, (new_index != current_index) ? 1 : 0);
            break;
        }
        case InputType::ButtonPress:
        case InputType::EncoderClick: {
            // Navigation back handled by softkeys or parent context
            break;
        }
        default:
            break;
    }
}

std::array<Softkey, NUM_SOFTKEYS> UISampleBrowser::getSoftkeys() {
    std::array<Softkey, NUM_SOFTKEYS> keys{};
    
    // Back button
    keys[0] = {"Back", [this]() { UINavigator::instance().pop(); }};
    
    // Audition/Stop button
    if (is_playing_) {
        keys[1] = {"Stop", [this]() {
            ESP_LOGI(TAG, "Stop audition requested");
            stopAudition();
        }};
    } else {
        keys[1] = {"Audition", [this]() {
            ESP_LOGI(TAG, "Audition requested");
            if (!file_browser_) {
                ESP_LOGW(TAG, "File browser not available");
                return;
            }
            
            const wavex_file_entry_t* selected = wavex_file_browser_get_selected(file_browser_);
            if (selected && !selected->is_directory) {
                uint32_t selected_index = wavex_file_browser_get_selected_index(file_browser_);
                auditionSampleByIndex(selected_index);
            } else {
                ESP_LOGW(TAG, "No valid file selected for audition");
            }
        }};
    }
    
    // Load button - handles directory traversal
    keys[2] = {"Load", [this]() {
        ESP_LOGI(TAG, "Load button pressed");
        if (!file_browser_) {
            ESP_LOGW(TAG, "File browser not available");
            return;
        }
        
        const wavex_file_entry_t* selected = wavex_file_browser_get_selected(file_browser_);
        if (!selected) {
            ESP_LOGW(TAG, "No entry selected");
            return;
        }
        
        // Check if it's ".." entry (parent directory)
        if (strcmp(selected->name, "..") == 0) {
            ESP_LOGI(TAG, "Navigating up to parent directory");
            wavex_file_browser_navigate_up(file_browser_);
            // Refresh softkeys after navigation
            refreshSoftkeys();
            return;
        }
        
        // Check if it's a directory
        if (selected->is_directory) {
            ESP_LOGI(TAG, "Navigating into directory: %s", selected->name);
            // Normalize path to avoid double slashes
            char normalized_path[96];
            const char* path_to_use = selected->path;
            
            // Remove any double slashes (but preserve root "/")
            if (path_to_use[0] == '/' && path_to_use[1] == '/') {
                normalized_path[0] = '/';
                int i = 2, j = 1;
                while (path_to_use[i] != '\0' && j < (int)sizeof(normalized_path) - 1) {
                    if (path_to_use[i] == '/' && normalized_path[j - 1] == '/') {
                        i++;
                        continue;
                    }
                    normalized_path[j++] = path_to_use[i++];
                }
                normalized_path[j] = '\0';
                path_to_use = normalized_path;
            }
            
            ESP_LOGI(TAG, "Normalized path: '%s' -> '%s'", selected->path, path_to_use);
            wavex_file_browser_navigate_to(file_browser_, path_to_use);
            // Refresh softkeys after navigation
            refreshSoftkeys();
        } else {
            // Regular file - load sample (existing functionality)
            ESP_LOGI(TAG, "Load sample requested for: %s", selected->name);
            // TODO: Implement actual sample loading
        }
    }};
    
    // Up arrow button
    keys[3] = {"Up", [this]() {
        ESP_LOGI(TAG, "Up button pressed");
        if (!file_browser_) return;
        wavex_file_browser_navigate_up_entry(file_browser_);
    }};
    
    // Down arrow button
    keys[4] = {"Down", [this]() {
        ESP_LOGI(TAG, "Down button pressed");
        if (!file_browser_) return;
        wavex_file_browser_navigate_down_entry(file_browser_);
    }};
    
    return keys;
}

void UISampleBrowser::file_selected_callback(const wavex_file_entry_t* entry, void* user_data) {
    UISampleBrowser* browser = static_cast<UISampleBrowser*>(user_data);
    if (!browser || !entry) return;
    
    ESP_LOGI(TAG, "File selected: %s", entry->name);
    browser->updateMetadata(entry);
    strncpy(browser->selected_file_path_, entry->path, sizeof(browser->selected_file_path_) - 1);
    browser->selected_file_path_[sizeof(browser->selected_file_path_) - 1] = '\0';
}

void UISampleBrowser::file_selected_index_callback(uint32_t file_index, const wavex_file_entry_t* entry, void* user_data) {
    UISampleBrowser* browser = static_cast<UISampleBrowser*>(user_data);
    if (!browser || !entry) return;
    
    ESP_LOGI(TAG, "File selected by index %lu: %s", (unsigned long)file_index, entry->name);
    browser->updateMetadata(entry);
    browser->selected_file_index_ = file_index;
    strncpy(browser->selected_file_path_, entry->name, sizeof(browser->selected_file_path_) - 1);
    browser->selected_file_path_[sizeof(browser->selected_file_path_) - 1] = '\0';
}

void UISampleBrowser::directory_changed_callback(const char* path, void* user_data) {
    UISampleBrowser* browser = static_cast<UISampleBrowser*>(user_data);
    if (!browser || !path) return;
    
    ESP_LOGI(TAG, "Directory changed to: %s", path);
    
    // Clear previous state
    browser->selected_file_index_ = 0;
    memset(browser->selected_file_path_, 0, sizeof(browser->selected_file_path_));
    
    // Stop any current audition when changing directories
    if (browser->is_playing_) {
        ESP_LOGI(TAG, "Stopping audition due to directory change");
        inter_mcu_send_sample_stop_req();
        browser->is_playing_ = false;
        
        // Refresh softkeys
        lv_async_call([](void* data) {
            UISampleBrowser* b = static_cast<UISampleBrowser*>(data);
            if (b) b->refreshSoftkeys();
        }, browser);
    }
    
    // Clear metadata display
    strcpy(browser->pending_metadata_text_, "Select a file to view metadata");
    browser->metadata_update_pending_ = true;
    browser->pending_metadata_entry_ = nullptr;
    wavex_ui_mark_content_changed();
    
    // Update status
    char status_text[256];
    snprintf(status_text, sizeof(status_text), "Browsing: %s", path);
    browser->updateStatus(status_text);
}

void UISampleBrowser::updateStatus(const char* status) {
    if (!status) return;
    
    // Store status text for deferred update (may be called from non-LVGL context)
    strncpy(pending_status_text_, status, sizeof(pending_status_text_) - 1);
    pending_status_text_[sizeof(pending_status_text_) - 1] = '\0';
    status_update_pending_ = true;
    wavex_ui_mark_content_changed();
    ESP_LOGI(TAG, "Status update queued: %s", status);
}

void UISampleBrowser::updateMetadata(const wavex_file_entry_t* entry) {
    if (!entry) return;
    
    // Store entry pointer for deferred update (may be called from non-LVGL context)
    pending_metadata_entry_ = entry;
    metadata_update_pending_ = true;
    wavex_ui_mark_content_changed();
    ESP_LOGI(TAG, "Metadata update queued for: %s", entry->name);
}

// Static method to process updates for active instance (called from UI task)
void UISampleBrowser::processDeferredUpdates() {
    if (s_active_instance_) {
        s_active_instance_->processDeferredUpdates_();
    }
}

void UISampleBrowser::processDeferredUpdates_() {
    // This should be called from UI task loop with LVGL lock held
    
    // Process status update
    if (status_update_pending_ && status_label_) {
        lv_label_set_text(status_label_, pending_status_text_);
        status_update_pending_ = false;
        ESP_LOGI(TAG, "Status label updated: %s", pending_status_text_);
    }
    
    // Process metadata update
    if (metadata_update_pending_ && metadata_label_) {
        char info_text[512];
        
        if (pending_metadata_entry_) {
            const wavex_file_entry_t* entry = pending_metadata_entry_;
            
            if (entry->is_directory) {
                snprintf(info_text, sizeof(info_text), 
                     "Directory Information:\n"
                     "─────────────────────\n"
                     "Name: %.47s\n"
                     "Type: Directory\n"
                     "Path: %.95s\n\n"
                     "Use Select to enter directory",
                     entry->name, entry->path);
            } else {
                // Format file size
                char size_str[32];
                if (entry->size_bytes < 1024) {
                    snprintf(size_str, sizeof(size_str), "%lu B", entry->size_bytes);
                } else if (entry->size_bytes < 1024 * 1024) {
                    snprintf(size_str, sizeof(size_str), "%.1f KB", entry->size_bytes / 1024.0f);
                } else {
                    snprintf(size_str, sizeof(size_str), "%.1f MB", entry->size_bytes / (1024.0f * 1024.0f));
                }
                
                // Format duration/sample rate/channels (placeholders for now)
                char duration_str[32] = "Unknown";
                char sample_rate_str[32] = "Unknown";
                char channels_str[32] = "Unknown";
                // TODO: Extract from WAV file metadata
                
                snprintf(info_text, sizeof(info_text), 
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
                     entry->name, size_str, duration_str, sample_rate_str, channels_str, entry->path);
            }
            
            lv_label_set_text(metadata_label_, info_text);
            metadata_update_pending_ = false;
            pending_metadata_entry_ = nullptr;
            ESP_LOGI(TAG, "Metadata label updated for: %s", entry->name);
        } else if (strlen(pending_metadata_text_) > 0) {
            lv_label_set_text(metadata_label_, pending_metadata_text_);
            metadata_update_pending_ = false;
            pending_metadata_text_[0] = '\0';
            ESP_LOGI(TAG, "Metadata label updated with pending text");
        }
    }
    
    // Process file browser pending updates
    if (file_browser_) {
        wavex_file_browser_process_pending_updates(file_browser_);
    }
}

bool UISampleBrowser::auditionSampleByIndex(uint32_t file_index) {
    ESP_LOGI(TAG, "=== SAMPLE PLAY INDEX OPERATION: About to audition sample by index: %lu ===", (unsigned long)file_index);
    
    esp_err_t result = inter_mcu_send_sample_play_index_req(file_index);
    if (result != ESP_OK) {
        ESP_LOGE(TAG, "Failed to send sample play index request: %d", result);
        return false;
    }
    
    ESP_LOGI(TAG, "=== SAMPLE PLAY INDEX OPERATION: Request sent successfully ===");
    is_playing_ = true;
    snprintf(selected_file_path_, sizeof(selected_file_path_), "Index %lu", (unsigned long)file_index);
    
    // Update status
    char status_text[256];
    snprintf(status_text, sizeof(status_text), "Playing: Index %lu", (unsigned long)file_index);
    updateStatus(status_text);
    
    // Refresh softkeys to reflect playing state
    refreshSoftkeys();
    
    return true;
}

bool UISampleBrowser::stopAudition() {
    if (!is_playing_) return false;
    
    ESP_LOGI(TAG, "=== SAMPLE STOP OPERATION: Stopping sample ===");
    
    esp_err_t result = inter_mcu_send_sample_stop_req();
    if (result != ESP_OK) {
        ESP_LOGE(TAG, "Failed to send sample stop request: %d", result);
        is_playing_ = false;
        updateStatus("Error: Stop failed");
        return false;
    }
    
    ESP_LOGI(TAG, "=== SAMPLE STOP OPERATION: Request sent successfully ===");
    
    // Don't immediately change is_playing - wait for response callback
    updateStatus("Stopping...");
    
    return true;
}

void UISampleBrowser::refreshSoftkeys() {
    // Use lv_async_call to ensure we're in LVGL context
    lv_async_call([](void* data) {
        UISampleBrowser* browser = static_cast<UISampleBrowser*>(data);
        if (browser) {
            UINavigator::instance().refreshSoftkeys();
        }
    }, this);
}

void UISampleBrowser::sample_status_callback(uint8_t state, uint32_t sample_rate, uint8_t channels, uint32_t frames_played, void* user_data) {
    UISampleBrowser* browser = static_cast<UISampleBrowser*>(user_data);
    if (!browser || browser != s_active_instance_) return;
    
    // State: 0 = stopped, 1 = playing, 2 = paused, etc.
    if (state == 0) {
        // Sample stopped
        ESP_LOGI(TAG, "=== SAMPLE STOP RESPONSE: Successfully stopped ===");
        browser->is_playing_ = false;
        browser->updateStatus("Stopped");
        browser->refreshSoftkeys();
    } else if (state == 1) {
        // Sample playing
        char status_text[256];
        snprintf(status_text, sizeof(status_text), "Playing: %lu Hz, %u ch, %lu frames", 
                 (unsigned long)sample_rate, channels, (unsigned long)frames_played);
        browser->updateStatus(status_text);
    }
}

std::shared_ptr<UIPage> createSampleBrowserPage() {
    return std::make_shared<UISampleBrowser>();
}

} // namespace wavex_ui
