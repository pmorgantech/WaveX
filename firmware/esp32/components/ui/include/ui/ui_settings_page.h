// WaveX UI Settings Page
#pragma once

#include <lvgl.h>

#include "input_event.h"
#include "ui_navigator.h"
#include "ui_page.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace wavex_ui {

/**
 * @brief Setting definition for settings pages
 */
struct Setting {
    std::string label;
    int value;
    int minValue;
    int maxValue;
    std::function<void(int)> onChange;
};

/**
 * @brief Settings page for parameter editing
 *
 * Displays a list of settings with encoder-based value adjustment.
 * Each setting has a label, current value, and min/max range.
 */
class UISettingsPage : public UIPage {
   public:
    /**
     * @brief Constructor
     * @param title Page title
     */
    explicit UISettingsPage(std::string title) : title_(std::move(title)) {}

    /**
     * @brief Get page name
     */
    const char* name() const override { return title_.c_str(); }

    /**
     * @brief Add a setting to the page
     * @param label Setting name
     * @param value Current value
     * @param minValue Minimum value
     * @param maxValue Maximum value
     * @param onChange Callback when value changes
     */
    void addSetting(const std::string& label,
                    int value,
                    int minValue,
                    int maxValue,
                    std::function<void(int)> onChange) {
        settings_.push_back({label, value, minValue, maxValue, std::move(onChange)});
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

   private:
    std::string title_;
    std::vector<Setting> settings_;
    int selectedSetting_ = 0;
    bool editingValue_ = false;

    lv_obj_t* list_ = nullptr;
    std::vector<lv_obj_t*> valueLabels_;

    /**
     * @brief Rebuild the settings list display
     */
    void rebuildList();

    /**
     * @brief Move selection up/down
     * @param delta Direction (+1 down, -1 up)
     */
    void moveSelection(int delta);

    /**
     * @brief Adjust the current setting value
     * @param delta Change amount (+1 increase, -1 decrease)
     */
    void adjustValue(int delta);

    /**
     * @brief Toggle between selection and value editing mode
     */
    void toggleEditMode();

    /**
     * @brief Update a setting's value
     * @param settingIndex Index of setting to update
     * @param newValue New value
     */
    void updateSetting(int settingIndex, int newValue);
};

}  // namespace wavex_ui
