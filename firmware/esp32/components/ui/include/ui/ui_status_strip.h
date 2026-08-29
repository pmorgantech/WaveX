// WaveX header status strip: output meters + engine CPU
#pragma once

#include <lvgl.h>

namespace wavex_ui {

/**
 * @brief Build the always-on status strip into the navigation header.
 *
 * Eight meter columns anchored to the right of the header, plus an engine-CPU
 * readout. Only the first two columns carry data today - the backend pushes a
 * stereo MeterPushMessage, not per-voice levels - so the remaining six render
 * as inactive stubs rather than fake activity.
 *
 * Creates its own LVGL timer; safe to call once, from LVGL context. Calling it
 * again replaces the strip.
 */
void statusStripCreate(lv_obj_t* header);

}  // namespace wavex_ui
