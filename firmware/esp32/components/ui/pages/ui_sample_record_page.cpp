#include "ui/ui_sample_record_page.h"

#include <esp_log.h>

#include "components/waveform_view.h"
#include "inter_mcu.h"
#include "ui/ui_navigator.h"

namespace wavex_ui {

namespace {
constexpr uint8_t kPreviewSlot = 0;
constexpr uint32_t kPreviewLength = 48000;  // 1 second at 48 kHz (with decimation)
constexpr uint16_t kPreviewDecim = 64;      // Down-sample on Daisy to lighten transport
static const char* TAG = "UI_SAMPLE_REC";
}  // namespace

void UISampleRecordPage::onEnter(lv_obj_t* parent) {
    root_ = lv_obj_create(parent);
    lv_obj_set_size(root_, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(root_, lv_color_make(0x00, 0x00, 0x00), LV_PART_MAIN);
    lv_obj_set_style_border_width(root_, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(root_, 10, LV_PART_MAIN);
    lv_obj_set_flex_flow(root_, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(root_, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    auto title = lv_label_create(root_);
    lv_label_set_text(title, "Sample Record");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_22, LV_PART_MAIN);
    lv_obj_set_style_text_color(title, lv_color_white(), LV_PART_MAIN);

    status_label_ = lv_label_create(root_);
    lv_label_set_text(status_label_, "Initializing...");
    lv_obj_set_style_text_color(status_label_, lv_color_white(), LV_PART_MAIN);
    lv_obj_align(status_label_, LV_ALIGN_TOP_LEFT, 0, 6);

    auto waveform_container = lv_obj_create(root_);
    lv_obj_set_size(waveform_container, lv_pct(100), 200);
    lv_obj_set_style_bg_color(waveform_container, lv_color_make(0x10, 0x10, 0x10), LV_PART_MAIN);
    lv_obj_set_style_border_width(waveform_container, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(
        waveform_container, lv_color_make(0x33, 0x33, 0x33), LV_PART_MAIN);
    lv_obj_set_style_pad_all(waveform_container, 4, LV_PART_MAIN);

    waveform_ = std::make_unique<WaveformView>(waveform_container, lv_pct(100), lv_pct(100));

    inter_mcu_set_wave_chunk_listener(&UISampleRecordPage::waveChunkStatic, this);
    requestWaveform();
    updateStatus("Ready");
}

void UISampleRecordPage::onExit() {
    inter_mcu_set_wave_chunk_listener(nullptr, nullptr);
    waveform_.reset();
    if (root_) {
        lv_obj_del(root_);
        root_ = nullptr;
    }
}

void UISampleRecordPage::onInput(const InputEvent& evt) {
    switch (evt.type) {
        case InputType::ButtonPress:
        case InputType::EncoderClick:
            toggleRecording();
            break;
        case InputType::EncoderLeft:
        case InputType::EncoderRight:
            requestWaveform();
            break;
        default:
            break;
    }
}

std::array<Softkey, NUM_SOFTKEYS> UISampleRecordPage::getSoftkeys() {
    std::array<Softkey, NUM_SOFTKEYS> keys{};
    keys[0] = {"Back", []() { UINavigator::instance().pop(); }};
    keys[1] = {is_recording_ ? "Stop" : "Record", [this]() { toggleRecording(); }};
    keys[2] = {"Refresh", [this]() { requestWaveform(); }};
    return keys;
}

void UISampleRecordPage::waveChunkStatic(uint32_t offset,
                                         const int16_t* samples,
                                         uint16_t count,
                                         void* user) {
    if (!user)
        return;
    static_cast<UISampleRecordPage*>(user)->handleWaveChunk(offset, samples, count);
}

void UISampleRecordPage::handleWaveChunk(uint32_t offset, const int16_t* samples, uint16_t count) {
    (void)offset;
    if (!samples || count == 0) {
        updateStatus("Waveform: no data");
        if (waveform_)
            waveform_->clear();
        return;
    }

    ESP_LOGI(TAG, "Wave chunk received: count=%u", (unsigned)count);
    if (waveform_) {
        waveform_->setSamples(samples, count);
    }
    updateStatus("Waveform updated");
}

void UISampleRecordPage::requestWaveform() {
    esp_err_t res = inter_mcu_send_preview_req(kPreviewSlot, 0, kPreviewLength, kPreviewDecim);
    if (res != ESP_OK) {
        updateStatus("Waveform request failed");
    } else {
        updateStatus("Requesting waveform...");
    }
}

void UISampleRecordPage::toggleRecording() {
    wavex_sample_ctrl_cmd_t cmd = is_recording_ ? WAVEX_SAMPLE_REC_STOP : WAVEX_SAMPLE_REC_START;
    esp_err_t res = inter_mcu_send_sample_ctrl(0, cmd, 1.0f);
    if (res != ESP_OK) {
        updateStatus(is_recording_ ? "Stop failed" : "Record start failed");
        return;
    }

    is_recording_ = !is_recording_;
    updateStatus(is_recording_ ? "Recording..." : "Stopped");
}

void UISampleRecordPage::updateStatus(const char* text) {
    if (status_label_) {
        lv_label_set_text(status_label_, text);
    }
}

std::shared_ptr<UIPage> createSampleRecordPage() {
    return std::make_shared<UISampleRecordPage>();
}

}  // namespace wavex_ui
