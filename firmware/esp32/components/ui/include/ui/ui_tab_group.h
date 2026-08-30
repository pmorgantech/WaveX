// WaveX tabbed page group
#pragma once

#include <lvgl.h>

namespace wavex_ui {

/**
 * @brief Creates a full-size tabview styled to the WaveX look.
 *
 * The house style for a tab group, in one place: a 56 px bar in montserrat_22,
 * inactive labels dimmed, and the selected tab drawn filled with white text
 * over a 4 px blue bottom border.
 *
 * This exists because that styling was previously inline in
 * ui_diagnostics_page.cpp, so every new tab group would have been a copy - and
 * copies of "the look" are what make two surfaces drift into looking almost the
 * same. `docs/ui-information-architecture.md` §2 pins the rule this serves:
 * tabs when the children share a subject, a menu list when they do not.
 *
 * Caller keeps ownership; delete the returned object with the rest of the page.
 * Must be called on the UI task, like all LVGL work.
 *
 * @param parent Container to fill.
 */
lv_obj_t* tabGroupCreate(lv_obj_t* parent);

/**
 * @brief Adds a styled tab page to a tabview from tabGroupCreate().
 *
 * The returned body is padding-free, border-free and non-scrollable, so a page
 * can position content against the tab's own top-left rather than fighting
 * LVGL's default container chrome.
 *
 * @param tabview Tabview from tabGroupCreate().
 * @param title   Bar label. Kept short: the bar divides evenly, so one long
 *                label shrinks every other tab's hit target.
 */
lv_obj_t* tabGroupAddTab(lv_obj_t* tabview, const char* title);

}  // namespace wavex_ui
