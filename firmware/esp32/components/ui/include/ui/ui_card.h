// WaveX card chrome
#pragma once

#include <lvgl.h>

namespace wavex_ui {

/**
 * @brief Applies the house drop shadow to a card-like container.
 *
 * Uses LVGL 9.5's native `drop_shadow_*` properties, which blur the object's
 * own alpha silhouette. These are a different feature from the older
 * `shadow_*` box-shadow properties, and they are not behind an `lv_conf` flag
 * - they are compiled into every image we build, so the only cost of calling
 * this is render time. `docs/roadmap.md` § 0.3 item 2 records why we took it
 * up and what still needs checking on the panel.
 *
 * Additive on purpose: it sets only the shadow properties and leaves fill,
 * border, radius and padding to the caller, so it can be dropped onto an
 * existing card without restyling it. Values live in ui_palette.h, in one
 * place, because a shadow copied per page is how two surfaces drift into
 * looking almost the same.
 *
 * Must be called on the UI task, like all LVGL work.
 *
 * @param card Container to shadow.
 */
void cardApplyDropShadow(lv_obj_t* card);

}  // namespace wavex_ui
