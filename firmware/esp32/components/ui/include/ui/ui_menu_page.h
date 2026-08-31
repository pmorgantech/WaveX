// WaveX UI Menu Page Class
#pragma once

#include <lvgl.h>

#include "input_event.h"
#include "ui_menu_item.h"
#include "ui_navigator.h"
#include "ui_page.h"

#include <memory>
#include <vector>

namespace wavex_ui {

/// Menu page for hierarchical navigation: a list of items with encoder/button
/// navigation, each opening a submenu, a page, or triggering an action.
class UIMenuPage : public UIPage {
   public:
    explicit UIMenuPage(std::string title) : title_(std::move(title)) {}

    const char* name() const override { return title_.c_str(); }

    void addItem(const std::string& label, std::function<void()> onSelect) {
        items_.push_back({label, std::move(onSelect)});
    }

    void onEnter(lv_obj_t* parent) override;
    void onExit() override;
    void onInput(const InputEvent& evt) override;
    std::array<Softkey, NUM_SOFTKEYS> getSoftkeys() override;

   protected:
    std::string title_;
    std::vector<MenuItem> items_;
    int selected_ = 0;
    lv_obj_t* list_ = nullptr;

    void rebuildList();
    void moveSelection(int delta);
    void activateSelection();
    static void list_event_cb(lv_event_t* e);
};

}  // namespace wavex_ui
