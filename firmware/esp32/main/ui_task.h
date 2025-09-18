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
 
 #ifdef __cplusplus
 }
 #endif