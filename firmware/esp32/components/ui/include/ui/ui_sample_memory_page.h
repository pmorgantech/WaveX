// WaveX UI Sample Memory Diagnostics Page
#pragma once

#include <lvgl.h>

#include "inter_mcu.h"
#include "ui_page.h"

#include <array>
#include <memory>
#include <string>

namespace wavex_ui {

/**
 * @brief Diagnostics page for sample RAM usage and loaded samples.
 */
class UISampleMemoryPage : public UIPage {
   public:
    const char* name() const override { return "Sample Memory"; }
    void onEnter(lv_obj_t* parent) override;
    void onExit() override;
    std::array<Softkey, NUM_SOFTKEYS> getSoftkeys() override;

   private:
    static void refresh_timer_cb(lv_timer_t* timer);
    void request_status();
    void refresh_ui();
    static void format_bytes(uint32_t bytes, char* out, size_t len);

    lv_obj_t* summary_label_ = nullptr;
    lv_obj_t* entries_label_ = nullptr;
    lv_timer_t* refresh_timer_ = nullptr;
    wavex_sample_mem_status_t status_cache_{};
};

std::shared_ptr<UIPage> createSampleMemoryPage();

}  // namespace wavex_ui

