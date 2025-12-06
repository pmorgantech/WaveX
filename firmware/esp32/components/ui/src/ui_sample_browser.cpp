// WaveX UI Sample Browser Implementation
#include "ui/ui_sample_browser.h"

#include <esp_log.h>

#include "../styles/ui_theme.h"
#include "comm/i_comm_interface.h"
#include "esp_lvgl_port.h"
#include "inter_mcu.h"
#include "ui_task.h"

static const char* TAG = "UI_SAMPLE_BROWSER";

namespace wavex_ui {

// Active instance for callbacks
UISampleBrowser* UISampleBrowser::s_active_instance_ = nullptr;

UISampleBrowser::UISampleBrowser(WaveX::Comm::ICommInterface& comm_interface,
                                 SampleBrowserState& persistent_state)
    : comm_interface_(&comm_interface), persistent_state_(persistent_state) {}

UISampleBrowser::~UISampleBrowser() {
    // Cleanup is done in onExit()
}

void UISampleBrowser::onEnter(lv_obj_t* parent) {
    // NOTE: onEnter is called from UINavigator::push/pop which already holds LVGL lock
    // No need to acquire lock here

    ESP_LOGI(TAG,
             "=== SAMPLE BROWSER ON_ENTER: Starting initialization, persistent_state.is_playing=%d",
             persistent_state_.is_playing ? 1 : 0);

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
    wavex_file_browser_config_t browser_config = {.root_path = persistent_state_.current_directory_path.c_str(), .file_extension = ".wav", .max_entries = 50, .show_hidden = false, .comm_interface = comm_interface_};

    ESP_LOGI(TAG, "Creating file browser with root_path: %s", browser_config.root_path);

    file_browser_ = wavex_file_browser_create(browser_container_, &browser_config);
    if (!file_browser_) {
        ESP_LOGE(TAG, "Failed to create file browser");
        return;
    }

    // Set callbacks
    wavex_file_browser_set_file_selected_callback(file_browser_, file_selected_callback, this);
    wavex_file_browser_set_file_selected_index_callback(
        file_browser_, file_selected_index_callback, this);
    wavex_file_browser_set_directory_changed_callback(
        file_browser_, directory_changed_callback, this);

    // Restore previous state
    is_playing_ = persistent_state_.is_playing;
    selected_file_index_ = persistent_state_.selected_file_index;
    current_directory_ = persistent_state_.current_directory_path;
    memset(selected_file_path_, 0, sizeof(selected_file_path_));

    ESP_LOGI(TAG,
             "=== STATE RESTORED: is_playing=%d, playing_path='%s', selected_index=%d",
             is_playing_ ? 1 : 0,
             persistent_state_.playing_sample_path.c_str(),
             selected_file_index_);

    // Set the selected index if we have entries
    if (file_browser_ && wavex_file_browser_get_entry_count(file_browser_) > 0) {
        wavex_file_browser_set_selection(file_browser_, persistent_state_.selected_file_index);

        // Get the selected entry and update metadata
        const wavex_file_entry_t* selected_entry = wavex_file_browser_get_selected(file_browser_);
        if (selected_entry) {
            updateMetadata(selected_entry);
            selected_file_index_ = persistent_state_.selected_file_index;
            strncpy(selected_file_path_, selected_entry->path, sizeof(selected_file_path_) - 1);
            selected_file_path_[sizeof(selected_file_path_) - 1] = '\0';
        }
    }

    // Register sample status callback and set active instance (after everything is initialized)
    ESP_LOGI(TAG, "=== SAMPLE BROWSER ON_ENTER: Registering callback and setting active instance");
    inter_mcu_set_sample_status_listener(sample_status_callback, this);
    s_active_instance_ = this;
    is_initialized_ = true;

    ESP_LOGI(TAG,
             "=== SAMPLE BROWSER ON_ENTER: Initialization complete, is_playing=%d",
             is_playing_ ? 1 : 0);
}

void UISampleBrowser::onExit() {
    // NOTE: onExit is called from UINavigator::push/pop which already holds LVGL lock
    // No need to acquire lock here

    ESP_LOGI(
        TAG, "=== SAMPLE BROWSER ON_EXIT: Starting cleanup, is_playing=%d", is_playing_ ? 1 : 0);

    // Immediately unregister callbacks and clear active instance to prevent any
    // callbacks from firing during cleanup or page transitions
    ESP_LOGI(TAG,
             "=== SAMPLE BROWSER ON_EXIT: Unregistering callback and clearing active instance");
    inter_mcu_set_sample_status_listener(nullptr, nullptr);
    if (s_active_instance_ == this) {
        s_active_instance_ = nullptr;
    }

    // Mark as not initialized to prevent any stray callbacks
    is_initialized_ = false;

    // NOTE: Do NOT stop playback when navigating away - allow playback to continue
    // This allows users to browse other pages while audio plays in the background

    // Preserve state before destroying
    if (file_browser_) {
        const char* current_path = wavex_file_browser_get_current_path(file_browser_);
        if (current_path) {
            persistent_state_.current_directory_path = current_path;
        }
        persistent_state_.selected_file_index =
            wavex_file_browser_get_selected_index(file_browser_);
        // Note: We keep the is_playing state as-is since playback continues across page navigation

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
            if (!file_browser_)
                break;

            ESP_LOGI(TAG,
                     "Received %s event for file browser scrolling (delta=%d)",
                     (evt.type == InputType::EncoderUp) ? "EncoderUp" : "EncoderDown",
                     evt.delta);

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
            ESP_LOGI(TAG,
                     "After nav: new_index=%u, moved=%d steps",
                     new_index,
                     (new_index != current_index) ? 1 : 0);
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
            stopAudition(); } };
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
            } } };
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
            // Regular file - load sample into sample RAM
            ESP_LOGI(TAG, "Load sample requested for: %s", selected->name);
            if (!loadSample(selected)) {
                ESP_LOGE(TAG, "Failed to load sample: %s", selected->name);
            }
        } } };

    // Up arrow button
    keys[3] = {"Up", [this]() {
        ESP_LOGI(TAG, "Up button pressed");
        if (!file_browser_) return;
        wavex_file_browser_navigate_up_entry(file_browser_); } };

    // Down arrow button
    keys[4] = {"Down", [this]() {
        ESP_LOGI(TAG, "Down button pressed");
        if (!file_browser_) return;
        wavex_file_browser_navigate_down_entry(file_browser_); } };

    return keys;
}

void UISampleBrowser::file_selected_callback(const wavex_file_entry_t* entry, void* user_data) {
    UISampleBrowser* browser = static_cast<UISampleBrowser*>(user_data);
    if (!browser || !entry)
        return;

    ESP_LOGI(TAG, "File selected: %s", entry->name);
    browser->updateMetadata(entry);
    strncpy(browser->selected_file_path_, entry->path, sizeof(browser->selected_file_path_) - 1);
    browser->selected_file_path_[sizeof(browser->selected_file_path_) - 1] = '\0';
}

void UISampleBrowser::file_selected_index_callback(uint32_t file_index,
                                                   const wavex_file_entry_t* entry,
                                                   void* user_data) {
    UISampleBrowser* browser = static_cast<UISampleBrowser*>(user_data);
    if (!browser || !entry)
        return;

    ESP_LOGI(TAG, "File selected by index %lu: %s", (unsigned long)file_index, entry->name);
    browser->updateMetadata(entry);
    browser->selected_file_index_ = file_index;
    browser->persistent_state_.selectFile(file_index, entry->name);
    strncpy(browser->selected_file_path_, entry->name, sizeof(browser->selected_file_path_) - 1);
    browser->selected_file_path_[sizeof(browser->selected_file_path_) - 1] = '\0';
}

void UISampleBrowser::directory_changed_callback(const char* path, void* user_data) {
    UISampleBrowser* browser = static_cast<UISampleBrowser*>(user_data);
    if (!browser || !path)
        return;

    ESP_LOGI(
        TAG, "Directory changed to: %s (current: %s)", path, browser->current_directory_.c_str());

    // Check if directory actually changed (not just refreshed)
    bool directory_actually_changed = (browser->current_directory_ != path);

    // Update current directory tracking
    browser->current_directory_ = path;

    // Update persistent state
    browser->persistent_state_.changeDirectory(path);

    // Clear previous state
    browser->selected_file_index_ = 0;
    memset(browser->selected_file_path_, 0, sizeof(browser->selected_file_path_));

    // Clear metadata display initially
    strcpy(browser->pending_metadata_text_, "Select a file to view metadata");
    browser->metadata_update_pending_ = true;
    browser->pending_metadata_entry_ = nullptr;
    wavex_ui_mark_content_changed();

    // Try to show metadata for currently selected file after directory loads
    // This will be called again when the browse response is fully processed
    lv_async_call(
        [](void* data) {
            UISampleBrowser* b = static_cast<UISampleBrowser*>(data);
            if (b && b->file_browser_ && b->is_initialized_) {
                uint32_t entry_count = wavex_file_browser_get_entry_count(b->file_browser_);
                if (entry_count > 0 && b->selected_file_index_ < entry_count) {
                    // Set the selection in the file browser
                    wavex_file_browser_set_selection(b->file_browser_, b->selected_file_index_);

                    // Get the selected entry and update metadata
                    const wavex_file_entry_t* selected_entry =
                        wavex_file_browser_get_selected(b->file_browser_);
                    if (selected_entry) {
                        b->updateMetadata(selected_entry);
            ESP_LOGI("UISampleBrowser",
                                 "Updated metadata for selected file: %s",
                                 selected_entry->name);
                    }
                }
            }
        },
        browser);

    // Update status - show playing status if we're currently playing, otherwise browsing
    char status_text[256];
    if (browser->is_playing_ && !browser->persistent_state_.playing_sample_path.empty()) {
        snprintf(status_text,
                 sizeof(status_text),
                 "Playing: %s",
                 browser->persistent_state_.playing_sample_path.c_str());
        ESP_LOGI(TAG,
                 "Directory changed: Showing playing status for %s",
                 browser->persistent_state_.playing_sample_path.c_str());
    } else {
        snprintf(status_text, sizeof(status_text), "Browsing: %s", path);
        ESP_LOGI(TAG,
                 "Directory changed: Showing browsing status (is_playing=%d, path_empty=%d)",
                 browser->is_playing_ ? 1 : 0,
                 browser->persistent_state_.playing_sample_path.empty() ? 1 : 0);
    }
    browser->updateStatus(status_text);
}

void UISampleBrowser::updateStatus(const char* status) {
    if (!status)
        return;

    // Safety check - don't queue updates if UI isn't ready
    if (!is_initialized_ || !status_label_ || !root_) {
        ESP_LOGW(TAG, "updateStatus called but UI not ready - status: %s", status);
        return;
    }

    // Store status text for deferred update (may be called from non-LVGL context)
    strncpy(pending_status_text_, status, sizeof(pending_status_text_) - 1);
    pending_status_text_[sizeof(pending_status_text_) - 1] = '\0';
    status_update_pending_ = true;
    wavex_ui_mark_content_changed();
    ESP_LOGI(TAG, "Status update queued: %s", status);
}

void UISampleBrowser::updateMetadata(const wavex_file_entry_t* entry) {
    if (!entry)
        return;

    // Safety check - don't queue updates if UI isn't ready
    if (!is_initialized_ || !metadata_label_ || !root_) {
        ESP_LOGW(TAG, "updateMetadata called but UI not ready - entry: %s", entry->name);
        return;
    }

    // Store entry pointer for deferred update (may be called from non-LVGL context)
    pending_metadata_entry_ = entry;
    metadata_update_pending_ = true;
    wavex_ui_mark_content_changed();
    ESP_LOGI(TAG, "Metadata update queued for: %s", entry->name);
}

// Static method to process updates for active instance (called from UI task)
void UISampleBrowser::processDeferredUpdates() {
    if (s_active_instance_) {
        ESP_LOGD(TAG, "processDeferredUpdates: calling processDeferredUpdates_()");
        s_active_instance_->processDeferredUpdates_();
    } else {
        ESP_LOGD(TAG, "processDeferredUpdates: no active instance");
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
                ESP_LOGI(TAG,
                         "=== METADATA: Directory - name='%s', path='%s'",
                         entry->name,
                         entry->path);
                snprintf(info_text,
                         sizeof(info_text),
                         "Directory Information:\n"
                         "---------------------\n"
                         "Name: %.47s\n"
                         "Type: Directory\n"
                         "Path: %.95s\n\n"
                         "Use Select to enter directory",
                         entry->name,
                         entry->path);
            } else {
                ESP_LOGI(TAG,
                         "=== METADATA: File - name='%s', size=%lu, path='%s'",
                         entry->name,
                         (unsigned long)entry->size_bytes,
                         entry->path);

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

                // Use WAV metadata from the file entry (provided by backend)
                char duration_str[32] = "Unknown";
                char sample_rate_str[32] = "Unknown";
                char channels_str[32] = "Unknown";

                if (entry->sample_rate > 0) {
                    // Format duration from milliseconds
                    uint32_t duration_ms = entry->duration_ms;
                    if (duration_ms >= 60000) {  // >= 1 minute
                        int minutes = duration_ms / 60000;
                        int seconds = (duration_ms % 60000) / 1000;
                        snprintf(duration_str, sizeof(duration_str), "%dm %ds", minutes, seconds);
                    } else if (duration_ms >= 1000) {
                        snprintf(
                            duration_str, sizeof(duration_str), "%.1fs", duration_ms / 1000.0f);
                    } else {
                        snprintf(duration_str, sizeof(duration_str), "%lums", duration_ms);
                    }

                    // Format sample rate
                    if (entry->sample_rate >= 1000) {
                        snprintf(sample_rate_str,
                                 sizeof(sample_rate_str),
                                 "%.1f kHz",
                                 entry->sample_rate / 1000.0f);
                    } else {
                        snprintf(sample_rate_str,
                                 sizeof(sample_rate_str),
                                 "%lu Hz",
                                 (unsigned long)entry->sample_rate);
                    }

                    // Format channels
                    if (entry->channels == 1) {
                        snprintf(channels_str, sizeof(channels_str), "Mono");
                    } else if (entry->channels == 2) {
                        snprintf(channels_str, sizeof(channels_str), "Stereo");
                    } else {
                        snprintf(channels_str, sizeof(channels_str), "%u ch", entry->channels);
                    }

                    ESP_LOGI(TAG,
                             "=== WAV METADATA: duration='%s', rate='%s', channels='%s', bits=%u",
                             duration_str,
                             sample_rate_str,
                             channels_str,
                             entry->bits_per_sample);
                } else {
                    ESP_LOGD(TAG, "No WAV metadata available for: %s", entry->name);
                }

                snprintf(info_text,
                         sizeof(info_text),
                         "Sample Information:\n"
                         "-----------------\n"
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

            ESP_LOGI(TAG,
                     "Setting metadata label text (length=%d): %.100s...",
                     strlen(info_text),
                     info_text);
            lv_label_set_text(metadata_label_, info_text);

            // Verify the label got the text
            const char* label_text = lv_label_get_text(metadata_label_);
            ESP_LOGI(TAG,
                     "Label text after set (length=%d): %.100s...",
                     label_text ? strlen(label_text) : 0,
                     label_text ? label_text : "NULL");

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
    ESP_LOGI(TAG,
             "=== SAMPLE PLAY INDEX OPERATION: About to audition sample by index: %lu, current "
             "is_playing=%d ===",
             (unsigned long)file_index,
             is_playing_ ? 1 : 0);

    esp_err_t result =
        comm_interface_ ? comm_interface_->sendSamplePlayRequest(file_index) : ESP_FAIL;
    if (result != ESP_OK) {
        ESP_LOGE(TAG, "Failed to send sample play index request: %d", result);
        return false;
    }

    ESP_LOGI(TAG, "=== SAMPLE PLAY INDEX OPERATION: Request sent successfully ===");
    is_playing_ = true;

    // Get the file name for status display
    const wavex_file_entry_t* entry = wavex_file_browser_get_entry(file_browser_, file_index);
    std::string filename = entry ? entry->name : "Unknown";

    persistent_state_.startPlayback(file_index, filename);
    snprintf(selected_file_path_, sizeof(selected_file_path_), "%s", filename.c_str());

    ESP_LOGI(TAG,
             "=== STARTED PLAYBACK: index=%d, filename='%s', persistent_path='%s'",
             file_index,
             filename.c_str(),
             persistent_state_.playing_sample_path.c_str());

    // Update status
    char status_text[256];
    snprintf(status_text, sizeof(status_text), "Playing: %s", filename.c_str());
    updateStatus(status_text);

    // Refresh softkeys to reflect playing state
    refreshSoftkeys();

    return true;
}

bool UISampleBrowser::stopAudition() {
    ESP_LOGI(TAG,
             "=== SAMPLE STOP OPERATION: Stopping sample, current is_playing=%d ===",
             is_playing_ ? 1 : 0);

    if (!is_playing_)
        return false;

    ESP_LOGI(TAG, "=== SAMPLE STOP OPERATION: Confirmed playing, proceeding with stop ===");

    esp_err_t result = comm_interface_ ? comm_interface_->sendSampleStopRequest() : ESP_FAIL;
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
    // Safety check - don't refresh if UI isn't ready
    if (!is_initialized_ || !root_) {
        ESP_LOGW(TAG, "refreshSoftkeys called but UI not ready");
        return;
    }

    // Use lv_async_call to ensure we're in LVGL context
    lv_async_call(
        [](void* data) {
            UISampleBrowser* browser = static_cast<UISampleBrowser*>(data);
            if (browser && browser->is_initialized_ && browser->root_) {
                UINavigator::instance().refreshSoftkeys();
            }
        },
        this);
}

void UISampleBrowser::sample_status_callback(uint8_t state,
                                             uint32_t sample_rate,
                                             uint8_t channels,
                                             uint32_t frames_played,
                                             void* user_data) {
    ESP_LOGI(
        TAG,
        "=== SAMPLE STATUS CALLBACK: state=%d, rate=%lu, channels=%u, frames=%lu, user_data=%p",
        state,
        (unsigned long)sample_rate,
        channels,
        (unsigned long)frames_played,
        user_data);

    UISampleBrowser* browser = static_cast<UISampleBrowser*>(user_data);

    // Comprehensive validation with detailed logging
    if (!browser) {
        ESP_LOGE(TAG, "=== CALLBACK ERROR: browser is NULL!");
        return;
    }

    if (browser != s_active_instance_) {
        ESP_LOGW(TAG,
                 "=== CALLBACK WARNING: browser=%p != active_instance=%p, ignoring",
                 browser,
                 s_active_instance_);
        return;
    }

    if (!browser->is_initialized_) {
        ESP_LOGW(TAG, "=== CALLBACK WARNING: browser not initialized, ignoring");
        return;
    }

    // Validate UI objects exist
    if (!browser->status_label_) {
        ESP_LOGE(TAG, "=== CALLBACK ERROR: status_label_ is NULL!");
        return;
    }

    if (!browser->root_) {
        ESP_LOGE(TAG, "=== CALLBACK ERROR: root_ is NULL!");
        return;
    }

    ESP_LOGI(TAG, "=== CALLBACK VALIDATION PASSED: Processing state=%d", state);

    // State: 0 = stopped, 1 = playing, 2 = paused, etc.
    if (state == 0) {
        ESP_LOGI(TAG, "=== SAMPLE STOP RESPONSE: Processing stop callback ===");
        browser->is_playing_ = false;
        browser->persistent_state_.stopPlayback();

        // Only update UI if we're still properly initialized
        if (browser->is_initialized_ && browser->status_label_ && browser->root_) {
            ESP_LOGI(TAG, "=== SAMPLE STOP RESPONSE: Updating UI ===");
            browser->updateStatus("Stopped");
            browser->refreshSoftkeys();
        } else {
            ESP_LOGW(TAG,
                     "=== SAMPLE STOP RESPONSE: Skipping UI update - not fully initialized ===");
        }
    } else if (state == 1) {
        ESP_LOGI(TAG, "=== SAMPLE PLAYING RESPONSE: Processing play callback ===");
        // Only update UI if we're still properly initialized
        if (browser->is_initialized_ && browser->status_label_ && browser->root_) {
            char status_text[256];
            snprintf(status_text,
                     sizeof(status_text),
                     "Playing: %lu Hz, %u ch, %lu frames",
                     (unsigned long)sample_rate,
                     channels,
                     (unsigned long)frames_played);
            browser->updateStatus(status_text);
        } else {
            ESP_LOGW(TAG,
                     "=== SAMPLE PLAYING RESPONSE: Skipping UI update - not fully initialized ===");
        }
    } else {
        ESP_LOGW(TAG, "=== UNKNOWN SAMPLE STATE: %d ===", state);
    }
}

bool UISampleBrowser::loadSample(const wavex_file_entry_t* entry) {
    ESP_LOGI(TAG, "=== SAMPLE LOAD OPERATION: Loading sample: %s ===", entry->name);

    if (!entry || strlen(entry->name) == 0) {
        ESP_LOGE(TAG, "Invalid file entry for sample loading");
        updateStatus("Error: Invalid file entry");
        return false;
    }

    // Gracefully fall back if metadata isn't available from the backend yet.
    // TODO(todo1): revert to strict validation once Daisy browse metadata is populated.
    uint32_t sample_rate = entry->sample_rate ? entry->sample_rate : 44100;
    uint8_t channels = entry->channels ? entry->channels : 2;
    uint8_t bits_per_sample = entry->bits_per_sample ? entry->bits_per_sample : 16;

    if (entry->sample_rate == 0 || entry->channels == 0 || entry->bits_per_sample == 0) {
        ESP_LOGW(TAG,
                 "Missing WAV metadata for %s (rate=%lu, ch=%u, bits=%u). "
                 "Using defaults %lu Hz, %u ch, %u-bit.",
                 entry->name,
                 (unsigned long)entry->sample_rate,
                 entry->channels,
                 entry->bits_per_sample,
                 (unsigned long)sample_rate,
                 channels,
                 bits_per_sample);
    }

    if (bits_per_sample != 16) {
        ESP_LOGE(TAG, "Unsupported bit depth: %u (only 16-bit supported)", bits_per_sample);
        updateStatus("Error: Only 16-bit WAV supported");
        return false;
    }

    // Daisy will load from its SD card; assign a unique sample ID per request
    uint16_t sample_id = persistent_state_.allocateSampleId();

    updateStatus("Loading sample on Daisy...");
    esp_err_t result = comm_interface_ ? inter_mcu_send_sample_load_req(sample_id,
                                                                        entry->size_bytes,
                                                                        sample_rate,
                                                                        channels,
                                                                        bits_per_sample,
                                                                        entry->path)
                                       : ESP_FAIL;

    if (result != ESP_OK) {
        ESP_LOGE(TAG, "Failed to send sample load request: %d", result);
        updateStatus("Error: Load request failed");
        return false;
    }

    ESP_LOGI(TAG,
             "=== SAMPLE LOAD REQUEST SENT TO DAISY: id=%u path=%s ===",
             (unsigned)sample_id,
             entry->path);
    updateStatus("Sample load requested on Daisy");

    return true;
}

std::shared_ptr<UIPage> createSampleBrowserPage(WaveX::Comm::ICommInterface& comm_interface) {
    // Create persistent state (static to maintain state across page instances)
    // This is better than static variables in the class because it's encapsulated
    // and could be moved to a state manager service in the future
    static SampleBrowserState persistent_state;
    return std::make_shared<UISampleBrowser>(comm_interface, persistent_state);
}

}  // namespace wavex_ui
