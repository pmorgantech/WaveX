// WaveX tabbed page group
#include "ui/ui_tab_group.h"

#include "ui_theme.h"

namespace wavex_ui {

lv_obj_t* tabGroupCreate(lv_obj_t* parent) {
    lv_obj_t* tabview = lv_tabview_create(parent);
    lv_tabview_set_tab_bar_size(tabview, UI_TAB_BAR_HEIGHT);
    lv_obj_set_size(tabview, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(tabview, UI_COLOR_BG, 0);
    lv_obj_set_style_border_width(tabview, 0, 0);
    lv_obj_set_style_pad_all(tabview, 0, 0);

    lv_obj_t* bar = lv_tabview_get_tab_bar(tabview);
    lv_obj_set_style_bg_color(bar, UI_COLOR_BG, 0);
    lv_obj_set_style_text_font(bar, UI_FONT_BODY, 0);
    lv_obj_set_style_text_color(bar, UI_COLOR_DIM, 0);
    // Inset to the same margin as page content, so the selected pill lines up
    // with the cards under it instead of running to the screen edge.
    lv_obj_set_style_pad_hor(bar, UI_MARGIN_X, 0);
    lv_obj_set_style_pad_top(bar, UI_TAB_BAR_PAD_TOP, 0);
    lv_obj_set_style_pad_column(bar, UI_GUTTER, 0);

    // Selected tab: an outlined card rather than a filled cell with a 4px
    // underline. The underline read as a progress bar next to the softkey
    // cards, and the fill competed with the focused card on the page below.
    const lv_style_selector_t sel_on = static_cast<lv_style_selector_t>(LV_PART_ITEMS) |
                                       static_cast<lv_style_selector_t>(LV_STATE_CHECKED);
    lv_obj_set_style_bg_color(bar, UI_COLOR_CARD_ALT, sel_on);
    lv_obj_set_style_text_color(bar, UI_COLOR_FG, sel_on);
    lv_obj_set_style_border_color(bar, UI_COLOR_ACCENT, sel_on);
    lv_obj_set_style_border_width(bar, UI_BORDER_WIDTH, sel_on);
    lv_obj_set_style_border_side(bar, LV_BORDER_SIDE_FULL, sel_on);
    lv_obj_set_style_radius(bar, UI_RADIUS_CHIP, sel_on);

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
    lv_obj_set_style_bg_color(tab, UI_COLOR_BG, 0);
    lv_obj_set_style_pad_all(tab, 0, 0);
    lv_obj_set_style_border_width(tab, 0, 0);
    lv_obj_remove_flag(tab, LV_OBJ_FLAG_SCROLLABLE);
    return tab;
}

}  // namespace wavex_ui
