#pragma once

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize the main WaveX UI system
 */
void ui_main_init(void);

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

#ifdef __cplusplus
}
#endif 