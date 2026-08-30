#include "ui/ui_card.h"

#include "ui/ui_palette.h"

namespace wavex_ui {

using namespace wavex_ui::palette;

void cardApplyDropShadow(lv_obj_t* card) {
    if (!card)
        return;

    lv_obj_set_style_drop_shadow_color(card, lv_color_hex(kShadowColor), 0);
    lv_obj_set_style_drop_shadow_opa(card, kShadowOpa, 0);
    lv_obj_set_style_drop_shadow_radius(card, kShadowRadius, 0);
    lv_obj_set_style_drop_shadow_offset_x(card, kShadowOffsetX, 0);
    lv_obj_set_style_drop_shadow_offset_y(card, kShadowOffsetY, 0);

    // SPEED, not AUTO. AUTO is free to pick the precise path, and this runs on
    // every card of every redraw on a rotated 1280x720 panel - the one place
    // where a "nicer" blur kernel is paid for continuously rather than once.
    lv_obj_set_style_drop_shadow_quality(card, LV_BLUR_QUALITY_SPEED, 0);
}

}  // namespace wavex_ui
