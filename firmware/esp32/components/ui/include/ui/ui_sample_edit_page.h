#pragma once

#include "input_event.h"
#include "ui_page.h"

#include <array>
#include <memory>
#include <vector>

namespace wavex_ui {

class UISampleEditPage : public UIPage {
   public:
    UISampleEditPage() = default;

    const char* name() const override { return "SampleEdit"; }

    void onEnter(lv_obj_t* parent) override;
    void onExit() override;
    void onInput(const InputEvent& evt) override;
    std::array<Softkey, NUM_SOFTKEYS> getSoftkeys() override;

   private:
    lv_obj_t* root_ = nullptr;
    lv_obj_t* status_label_ = nullptr;
    std::unique_ptr<class WaveformView> waveform_;
    bool has_sample_ = false;
    std::vector<int16_t> preview_buffer_;
    uint32_t expected_len_ = 0;

    static void waveChunkStatic(uint32_t offset,
                                const int16_t* samples,
                                uint16_t count,
                                void* user);
    void handleWaveChunk(uint32_t offset, const int16_t* samples, uint16_t count);

    void requestWaveform();
    void refreshStatus(const char* text);
};

std::shared_ptr<UIPage> createSampleEditPage();

}  // namespace wavex_ui
