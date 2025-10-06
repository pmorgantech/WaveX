// WaveX UI Existing Pages Integration
#pragma once

#include "ui_page.h"
#include "ui_navigator.h"
#include "input_event.h"
#include <lvgl.h>
#include <memory>

namespace wavex_ui {

/**
 * @brief Diagnostics page wrapper for navigation system
 * 
 * Wraps the existing diagnostics page to work with the navigation system.
 */
class UIDiagnosticsPage : public UIPage {
public:
    const char* name() const override { return "Diagnostics"; }

    void onEnter(lv_obj_t* parent) override;
    void onExit() override;
    void onInput(const InputEvent& evt) override;
    std::array<Softkey, NUM_SOFTKEYS> getSoftkeys() override;

private:
    lv_obj_t* diagnostics_container_ = nullptr;
};

/**
 * @brief Sample Load/Save page wrapper for navigation system
 * 
 * Wraps the existing sample load/save page to work with the navigation system.
 */
class UISampleLoadSavePage : public UIPage {
public:
    const char* name() const override { return "Sample Load/Save"; }

    void onEnter(lv_obj_t* parent) override;
    void onExit() override;
    void onInput(const InputEvent& evt) override;
    std::array<Softkey, NUM_SOFTKEYS> getSoftkeys() override;

private:
    lv_obj_t* sample_container_ = nullptr;
    void* sample_page_handle_ = nullptr; // Handle to the existing page
};

/**
 * @brief Factory functions for existing pages
 */
std::shared_ptr<UIPage> createDiagnosticsPage();
std::shared_ptr<UIPage> createSampleLoadSavePage();

} // namespace wavex_ui
