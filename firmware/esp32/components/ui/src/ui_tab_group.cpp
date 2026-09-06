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

    return tabview;
}

// Style one tab button. LVGL 9.5's tabview bar holds real lv_button children
// rather than a button matrix, so LV_PART_ITEMS on the bar styles nothing and
// each button keeps the default theme's own blue - which is invisible while
// the accent happens to be blue too, and obvious the moment a theme changes
// it. Styling the button directly is what actually reaches it.
static void styleTabButton(lv_obj_t* btn) {
    if (!btn) {
        return;
    }
    const lv_style_selector_t checked = static_cast<lv_style_selector_t>(LV_STATE_CHECKED);

    lv_obj_set_style_bg_opa(btn, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(btn, 0, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(btn, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(btn, UI_RADIUS_CHIP, LV_PART_MAIN);
    lv_obj_set_style_text_color(btn, UI_COLOR_DIM, LV_PART_MAIN);
    lv_obj_set_style_text_font(btn, UI_FONT_BODY, LV_PART_MAIN);

    // Selected: an outlined card rather than a filled cell with a 4px
    // underline. The underline read as a progress bar next to the softkey
    // cards, and the fill competed with the focused card on the page below.
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, LV_PART_MAIN | checked);
    lv_obj_set_style_bg_color(btn, UI_COLOR_CARD_ALT, LV_PART_MAIN | checked);
    lv_obj_set_style_border_width(btn, UI_BORDER_WIDTH, LV_PART_MAIN | checked);
    lv_obj_set_style_border_color(btn, UI_COLOR_ACCENT, LV_PART_MAIN | checked);
    lv_obj_set_style_border_side(btn, LV_BORDER_SIDE_FULL, LV_PART_MAIN | checked);
    lv_obj_set_style_text_color(btn, UI_COLOR_FG, LV_PART_MAIN | checked);
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

    // The button this tab just added is the bar's last child.
    lv_obj_t* bar = lv_tabview_get_tab_bar(tabview);
    if (bar) {
        const uint32_t n = lv_obj_get_child_count(bar);
        if (n > 0) {
            styleTabButton(lv_obj_get_child(bar, static_cast<int32_t>(n - 1)));
        }
    }
    return tab;
}

}  // namespace wavex_ui
