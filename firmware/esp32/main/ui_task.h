/**
 * @file ui_task.h
 * @brief UI Task for MIPI DSI Display with LVGL
 * 
 * This header provides the interface for the UI task that manages
 * MIPI DSI display, LVGL UI system, and GT911 touch controller.
 */

 #pragma once

 #include "esp_err.h"
 #include "esp_lcd_types.h"
 #include "lvgl.h"
 
 #ifdef __cplusplus
 extern "C" {
 #endif
 
 /**
  * @brief Start the UI task with MIPI DSI display and LVGL
  * 
  * Initializes the MIPI DSI display with HX8394 driver, GT911 touch controller,
  * and LVGL UI system for the Waveshare 5-DSI-TOUCH-A display.
  * 
  * @return ESP_OK on success, error code otherwise
  */
 esp_err_t wavex_ui_task_start(void);
 
 /**
  * @brief Stop the UI task
  * 
  * Deinitializes all UI components including display, touch, and LVGL.
  * 
  * @return ESP_OK on success, error code otherwise
  */
 esp_err_t wavex_ui_task_stop(void);
 
 /**
  * @brief Get handle to the LCD panel
  * 
  * @param[out] panel_handle Handle to the LCD panel
  * @return ESP_OK on success, error code otherwise
  */
 esp_err_t wavex_ui_get_panel_handle(esp_lcd_panel_handle_t *panel_handle);
 
 /**
  * @brief Create the enhanced meter display
  * 
  * @param parent The parent LVGL object
  */
 void wavex_ui_create_meter_display(lv_obj_t *parent);
 
 /**
  * @brief Set the current screen context for hotkey mapping
  * 
  * @param screen_name The name of the current screen
  */
 void wavex_ui_set_screen_context(const char* screen_name);
 
 /**
  * @brief Update the header title
  * 
  * @param title The new title
  */
 void wavex_ui_update_header_title(const char* title);
 
/**
 * @brief Update hotkey labels
 * 
 * @param labels Array of 6 strings for the hotkey labels
 */
void wavex_ui_update_hotkey_labels(const char* labels[6]);

/**
 * @brief Update hotkey label for a specific button
 * 
 * @param button_index Index of the button (0-5)
 * @param label New label text
 */
void wavex_ui_update_hotkey_label(int button_index, const char* label);
 
 /**
  * @brief Mark UI content as changed to trigger a refresh
  */
 void wavex_ui_mark_content_changed(void);
 
 #ifdef __cplusplus
 }
 #endif