#pragma once

#include "input_event.h"
#include "ui_page.h"

#include <array>
#include <memory>
#include <vector>

namespace wavex_ui {

class UISampleRecordPage : public UIPage {
   public:
    UISampleRecordPage() = default;

    const char* name() const override { return "SampleRecord"; }

    void onEnter(lv_obj_t* parent) override;
    void onExit() override;
    void onInput(const InputEvent& evt) override;
    std::array<Softkey, NUM_SOFTKEYS> getSoftkeys() override;

   private:
    lv_obj_t* root_ = nullptr;
    lv_obj_t* status_label_ = nullptr;
    std::unique_ptr<class WaveformView> waveform_;
    bool is_recording_ = false;

    static void waveChunkStatic(uint32_t offset,
                                const int16_t* samples,
                                uint16_t count,
                                void* user);
    void handleWaveChunk(uint32_t offset, const int16_t* samples, uint16_t count);

    void requestWaveform();
    void toggleRecording();
    void updateStatus(const char* text);
};

std::shared_ptr<UIPage> createSampleRecordPage();

}  // namespace wavex_ui
