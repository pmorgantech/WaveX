// WaveX tabbed page group
#include "ui/ui_tab_group.h"

#include "ui/ui_palette.h"

namespace wavex_ui {

using namespace wavex_ui::palette;

lv_obj_t* tabGroupCreate(lv_obj_t* parent) {
    lv_obj_t* tabview = lv_tabview_create(parent);
    lv_tabview_set_tab_bar_size(tabview, 56);
    lv_obj_set_size(tabview, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(tabview, lv_color_hex(kColBg), 0);
    lv_obj_set_style_border_width(tabview, 0, 0);
    lv_obj_set_style_pad_all(tabview, 0, 0);

    lv_obj_t* bar = lv_tabview_get_tab_bar(tabview);
    lv_obj_set_style_bg_color(bar, lv_color_hex(kColBg), 0);
    lv_obj_set_style_text_font(bar, &lv_font_montserrat_22, 0);
    lv_obj_set_style_text_color(bar, lv_color_hex(kColDimmer), 0);

    // Selected tab cell: filled, white text, 4 px blue underline. The selector
    // is composed once because mixing lv_part_t with lv_state_t directly warns
    // under C++20, which this component is built with.
    const lv_style_selector_t sel_on = static_cast<lv_style_selector_t>(LV_PART_ITEMS) |
                                       static_cast<lv_style_selector_t>(LV_STATE_CHECKED);
    lv_obj_set_style_bg_color(bar, lv_color_hex(kColTabOn), sel_on);
    lv_obj_set_style_text_color(bar, lv_color_white(), sel_on);
    lv_obj_set_style_border_color(bar, lv_color_hex(kColBlue), sel_on);
    lv_obj_set_style_border_width(bar, 4, sel_on);
    lv_obj_set_style_border_side(bar, LV_BORDER_SIDE_BOTTOM, sel_on);

    return tabview;
}

lv_obj_t* tabGroupAddTab(lv_obj_t* tabview, const char* title) {
    if (!tabview) {
        return nullptr;
    }
    lv_obj_t* tab = lv_tabview_add_tab(tabview, title);
    if (!tab) {
        return nullptr;
    }
    lv_obj_set_style_bg_color(tab, lv_color_hex(kColBg), 0);
    lv_obj_set_style_pad_all(tab, 0, 0);
    lv_obj_set_style_border_width(tab, 0, 0);
    lv_obj_remove_flag(tab, LV_OBJ_FLAG_SCROLLABLE);
    return tab;
}

}  // namespace wavex_ui
