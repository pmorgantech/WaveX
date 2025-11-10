// WaveX UI Sample Detail Page
#pragma once

#include <lvgl.h>

#include "input_event.h"
#include "ui_navigator.h"
#include "ui_page.h"

#include <memory>
#include <string>

namespace wavex_ui {

/**
 * @brief Sample detail page showing file information
 *
 * Displays sample metadata and provides playback controls.
 * Simple page that demonstrates the navigation system.
 */
class UISampleDetail : public UIPage {
   public:
    /**
     * @brief Constructor
     * @param filename Name of the sample file
     */
    explicit UISampleDetail(std::string filename) : filename_(std::move(filename)) {}

    /**
     * @brief Get page name
     */
    const char* name() const override { return "SampleDetail"; }

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
    std::string filename_;
    lv_obj_t* infoLabel_ = nullptr;
    bool isPlaying_ = false;

    /**
     * @brief Update the info display
     */
    void updateInfo();

    /**
     * @brief Start/stop sample playback
     */
    void togglePlayback();
};

/**
 * @brief Factory function to create sample detail page
 */
std::shared_ptr<UIPage> createSampleDetailPage(const std::string& filename);

}  // namespace wavex_ui
