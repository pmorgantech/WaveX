/**
 * @file window_manager.cpp
 * @brief Window Manager Implementation
 */

#include "window_manager.h"

#include "../styles/ui_theme.h"
#include "esp_log.h"

static const char* TAG = "WINDOW_MANAGER";

// Forward declarations
static void hotkey_button_event_cb(lv_event_t* e);

wavex_window_t* wavex_window_create(lv_obj_t* parent, const char* title) {
    if (!parent) {
        ESP_LOGE(TAG, "Parent is NULL");
        return NULL;
    }

    wavex_window_t* window = (wavex_window_t*)malloc(sizeof(wavex_window_t));
    if (!window) {
        ESP_LOGE(TAG, "Failed to allocate window structure");
        return NULL;
    }

    // Initialize structure
    memset(window, 0, sizeof(wavex_window_t));

    // Create main window container
    window->window = lv_obj_create(parent);
    if (!window->window) {
        ESP_LOGE(TAG, "Failed to create window container");
        free(window);
        return NULL;
    }

    lv_obj_set_size(window->window, lv_pct(100), lv_pct(100));
    ui_theme_apply_container_style(window->window, false);
    lv_obj_align(window->window, LV_ALIGN_TOP_LEFT, 0, 0);

    // Create header
    window->header = lv_obj_create(window->window);
    lv_obj_set_size(window->header, lv_pct(100), UI_HEADER_HEIGHT);
    lv_obj_set_style_bg_color(window->header, UI_COLOR_HEADER, LV_PART_MAIN);
    lv_obj_set_style_border_width(window->header, 0, LV_PART_MAIN);
    lv_obj_align(window->header, LV_ALIGN_TOP_MID, 0, 0);

    // Create title label
    window->title_label = lv_label_create(window->header);
    lv_label_set_text(window->title_label, title ? title : "");
    lv_obj_set_style_text_color(window->title_label, UI_COLOR_TEXT, LV_PART_MAIN);
    lv_obj_set_style_text_font(window->title_label, UI_FONT_HEADER, LV_PART_MAIN);
    lv_obj_center(window->title_label);

    // Create content area
    window->content = lv_obj_create(window->window);
    lv_obj_set_size(
        window->content, lv_pct(100), lv_pct(100) - UI_HEADER_HEIGHT - UI_HOTKEY_HEIGHT);
    ui_theme_apply_container_style(window->content, false);
    lv_obj_align(window->content, LV_ALIGN_TOP_MID, 0, UI_HEADER_HEIGHT);

    // Create hotkey region
    window->hotkey_region = lv_obj_create(window->window);
    lv_obj_set_size(window->hotkey_region, lv_pct(100), UI_HOTKEY_HEIGHT);
    lv_obj_set_style_bg_color(window->hotkey_region, UI_COLOR_HOTKEY, LV_PART_MAIN);
    lv_obj_set_style_border_width(window->hotkey_region, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(window->hotkey_region, UI_COLOR_BORDER, LV_PART_MAIN);
    lv_obj_set_style_pad_all(window->hotkey_region, UI_PADDING_SMALL, LV_PART_MAIN);
    lv_obj_align(window->hotkey_region, LV_ALIGN_BOTTOM_MID, 0, 0);

    // Set flex layout for hotkey buttons
    lv_obj_set_flex_flow(window->hotkey_region, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(window->hotkey_region, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    ESP_LOGI(TAG, "Window created successfully with title: %s", title ? title : "Untitled");
    return window;
}

void wavex_window_destroy(wavex_window_t* window) {
    if (!window)
        return;

    // LVGL will handle object cleanup when parent is deleted
    if (window->window) {
        lv_obj_del(window->window);
    }

    free(window);
    ESP_LOGI(TAG, "Window destroyed");
}

void wavex_window_set_hotkeys(wavex_window_t* window, const wavex_hotkey_config_t* config) {
    if (!window || !config)
        return;

    // Clear existing hotkey buttons
    lv_obj_clean(window->hotkey_region);

    // Count active labels
    int num_active_labels = 0;
    for (int i = 0; i < 6; i++) {
        if (config->labels[i] && strlen(config->labels[i]) > 0) {
            num_active_labels++;
        }
    }

    if (num_active_labels == 0) {
        ESP_LOGI(TAG, "No active hotkey labels provided");
        return;
    }

    // Calculate button widths
    int slots_per_button = 6 / num_active_labels;
    int remaining_slots = 6 % num_active_labels;

    int button_index = 0;
    for (int i = 0; i < 6; i++) {
        if (config->labels[i] && strlen(config->labels[i]) > 0) {
            // Create button
            lv_obj_t* button = lv_btn_create(window->hotkey_region);

            // Calculate width
            int slots = slots_per_button + (button_index < remaining_slots ? 1 : 0);
            int width_percent = (slots * 100) / 6;
            lv_obj_set_size(button, lv_pct(width_percent), 90);

            // Apply styling
            ui_theme_apply_button_style(button, true);

            // Add event callback
            lv_obj_add_event_cb(button,
                                config->callback ? config->callback : hotkey_button_event_cb,
                                LV_EVENT_CLICKED,
                                (void*)(intptr_t)i);

            // Create label
            lv_obj_t* label = lv_label_create(button);
            lv_label_set_text(label, config->labels[i]);
            lv_obj_set_style_text_font(label, UI_FONT_HOTKEY, LV_PART_MAIN);
            lv_obj_set_style_text_color(label, UI_COLOR_TEXT, LV_PART_MAIN);
            lv_obj_center(label);

            // Store user data if provided
            if (config->user_data[i]) {
                lv_obj_set_user_data(button, config->user_data[i]);
            }

            button_index++;
        }
    }

    ESP_LOGI(TAG, "Set %d hotkey buttons", num_active_labels);
}

void wavex_window_clear_hotkeys(wavex_window_t* window) {
    if (!window)
        return;

    lv_obj_clean(window->hotkey_region);
    ESP_LOGI(TAG, "Cleared hotkey buttons");
}

void wavex_window_clear_content(wavex_window_t* window) {
    if (!window)
        return;

    lv_obj_clean(window->content);
    ESP_LOGI(TAG, "Cleared window content");
}

void wavex_window_set_title(wavex_window_t* window, const char* title) {
    if (!window || !window->title_label)
        return;

    lv_label_set_text(window->title_label, title ? title : "");
    ESP_LOGI(TAG, "Set window title: %s", title ? title : "Untitled");
}

void wavex_window_update_layout(wavex_window_t* window) {
    if (!window)
        return;

    // Update content area height to account for header and hotkey region
    lv_obj_set_size(
        window->content, lv_pct(100), lv_pct(100) - UI_HEADER_HEIGHT - UI_HOTKEY_HEIGHT);
    lv_obj_align(window->content, LV_ALIGN_TOP_MID, 0, UI_HEADER_HEIGHT);

    ESP_LOGD(TAG, "Updated window layout");
}

// Default hotkey button event callback
static void hotkey_button_event_cb(lv_event_t* e) {
    lv_event_code_t code = lv_event_get_code(e);
    int button_index = (int)(intptr_t)lv_event_get_user_data(e);

    if (code == LV_EVENT_CLICKED) {
        ESP_LOGI(TAG, "Hotkey button %d clicked", button_index);
        // Default implementation - can be overridden by specific pages
    }
}
