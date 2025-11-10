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

/**
 * @brief Menu page for hierarchical navigation
 *
 * Displays a list of menu items with encoder/button navigation.
 * Each item can open a submenu, functional page, or trigger an action.
 */
class UIMenuPage : public UIPage {
   public:
    /**
     * @brief Constructor
     * @param title Menu title displayed at the top
     */
    explicit UIMenuPage(std::string title) : title_(std::move(title)) {}

    /**
     * @brief Get page name
     */
    const char* name() const override { return title_.c_str(); }

    /**
     * @brief Add a menu item
     * @param label Display text
     * @param onSelect Callback when selected
     */
    void addItem(const std::string& label, std::function<void()> onSelect) {
        items_.push_back({label, std::move(onSelect)});
    }

    /**
     * @brief Called when page becomes active
     */
    void onEnter(lv_obj_t* parent) override;

    /**
     * @brief Called when page becomes inactive
     */
    void onExit() override;

    /**
     * @brief Handle input events
     */
    void onInput(const InputEvent& evt) override;

    /**
     * @brief Get softkey configuration
     */
    std::array<Softkey, NUM_SOFTKEYS> getSoftkeys() override;

   protected:
    std::string title_;
    std::vector<MenuItem> items_;
    int selected_ = 0;
    lv_obj_t* list_ = nullptr;

    /**
     * @brief Rebuild the menu list display
     */
    void rebuildList();

    /**
     * @brief Move selection up/down
     * @param delta Direction (+1 down, -1 up)
     */
    void moveSelection(int delta);

    /**
     * @brief Activate the currently selected item
     */
    void activateSelection();

    /**
     * @brief Handle touch events on list items
     */
    static void list_event_cb(lv_event_t* e);
};

}  // namespace wavex_ui
