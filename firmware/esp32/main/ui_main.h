#pragma once

#include "lvgl.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize the main WaveX UI system
 */
void wavex_ui_init(void);

/**
 * @brief Create the main menu structure
 * @return Pointer to the created menu object
 */
lv_obj_t* ui_main_create_menu(void);

/**
 * @brief Get the main screen object
 * @return Pointer to the main screen
 */
lv_obj_t* ui_main_get_screen(void);

/**
 * @brief Create system diagnostics page
 * @param parent_menu Parent menu object
 * @return Pointer to the created diagnostics page
 */
lv_obj_t* ui_diagnostics_create(lv_obj_t* parent_menu);

/**
 * @brief Create setup/configuration page
 * @param parent_menu Parent menu object
 * @return Pointer to the created setup page
 */
lv_obj_t* ui_setup_create(lv_obj_t* parent_menu);

/**
 * @brief Update system resource monitoring widgets
 */
void ui_update_system_resources(void);

esp_err_t wavex_hardware_init(void);

#ifdef __cplusplus
}
#endif 