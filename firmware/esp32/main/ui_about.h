#pragma once

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize the About screen UI component
 * @return Pointer to the created About menu page
 */
lv_obj_t* ui_about_create(lv_obj_t* parent_menu);

/**
 * @brief Create and show the license information modal
 * @param parent Parent object for the modal
 */
void ui_about_show_license_modal(lv_obj_t* parent);

/**
 * @brief Get firmware version information as formatted string
 * @param buffer Buffer to store the version string
 * @param buffer_size Size of the buffer
 */
void ui_about_get_version_info(char* buffer, size_t buffer_size);

#ifdef __cplusplus
}
#endif 