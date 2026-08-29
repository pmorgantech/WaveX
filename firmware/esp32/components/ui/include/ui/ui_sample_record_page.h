#pragma once

#include "input_event.h"
#include "ui_page.h"

#include <array>
#include <atomic>
#include <cstdint>
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
    // A wave chunk is bounded by the UART payload limit (2048 bytes, minus the
    // WaveChunkMessage header, as int16 samples). Staging the whole chunk means
    // the render sees exactly what arrived rather than a truncated prefix.
    static constexpr uint16_t kWaveStageCapacity = 1021;

    lv_obj_t* root_ = nullptr;
    lv_obj_t* status_label_ = nullptr;
    lv_timer_t* ui_timer_ = nullptr;
    std::unique_ptr<class WaveformView> waveform_;
    bool is_recording_ = false;

    // Cross-task staging. handleWaveChunk() and the status updates it makes run
    // on the UART RX task; nothing there may touch a widget, because the LVGL
    // task renders on the other core. serviceUi() drains these on the UI task.
    // The pending flags are release-stored after their payload and
    // acquire-loaded before it, so a raised flag always implies visible data.
    int16_t staged_samples_[kWaveStageCapacity] = {};
    uint16_t staged_count_ = 0;
    std::atomic<bool> wave_pending_{false};
    std::atomic<bool> wave_clear_pending_{false};
    char staged_status_[64] = {0};
    std::atomic<bool> status_pending_{false};

    static void waveChunkStatic(uint32_t offset,
                                const int16_t* samples,
                                uint16_t count,
                                void* user);
    void handleWaveChunk(uint32_t offset, const int16_t* samples, uint16_t count);

    static void uiTimerCb(lv_timer_t* timer);
    void serviceUi();

    void requestWaveform();
    void toggleRecording();
    void updateStatus(const char* text);
};

std::shared_ptr<UIPage> createSampleRecordPage();

}  // namespace wavex_ui
