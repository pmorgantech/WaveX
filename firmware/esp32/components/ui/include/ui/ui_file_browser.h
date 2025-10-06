// WaveX UI File Browser Page
#pragma once

#include "ui_page.h"
#include "ui_navigator.h"
#include "input_event.h"
#include <lvgl.h>
#include <vector>
#include <string>
#include <memory>

namespace wavex_ui {

/**
 * @brief File browser page with persistent state
 * 
 * Maintains current directory, file list, and selection state
 * between visits using the navigation stack.
 */
class UIFileBrowser : public UIPage {
public:
    /**
     * @brief Constructor
     * @param initialPath Starting directory path
     */
    explicit UIFileBrowser(std::string initialPath = "/samples")
        : currentPath_(std::move(initialPath)) {}

    /**
     * @brief Get page name
     */
    const char* name() const override { return "FileBrowser"; }

    /**
     * @brief Called when page becomes active
     */
    void onEnter(lv_obj_t* parent) override;

    /**
     * @brief Called when page becomes inactive
     * Preserves state for return visits
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
    std::string currentPath_;
    std::vector<std::string> files_;
    int selected_ = 0;

    lv_obj_t* labelPath_ = nullptr;
    lv_obj_t* list_ = nullptr;

    /**
     * @brief Refresh the file list display
     */
    void refreshList();

    /**
     * @brief Move selection up/down
     * @param delta Direction (+1 down, -1 up)
     */
    void moveSelection(int delta);

    /**
     * @brief Open the selected file or directory
     */
    void openSelection();

    /**
     * @brief Navigate up one directory level
     */
    void navigateUp();

    /**
     * @brief Load files for current directory
     */
    void loadFiles();
};

/**
 * @brief Factory function to create file browser page
 */
std::shared_ptr<UIPage> createFileBrowserPage(const std::string& path = "/samples");

} // namespace wavex_ui
