// WaveX UI Sample Detail Implementation
#include "ui/ui_sample_detail.h"
#include <esp_log.h>

static const char* TAG = "UI_SAMPLE_DETAIL";

namespace wavex_ui {

void UISampleDetail::onEnter(lv_obj_t* parent) {
    root_ = lv_obj_create(parent);
    lv_obj_set_size(root_, 480, 320);
    lv_obj_set_style_bg_color(root_, lv_color_make(0x00, 0x00, 0x00), LV_PART_MAIN); // Dark mode
    lv_obj_set_style_border_width(root_, 0, LV_PART_MAIN);
    lv_obj_align(root_, LV_ALIGN_TOP_LEFT, 0, 0);

    // Create title
    auto titleLabel = lv_label_create(root_);
    lv_label_set_text(titleLabel, "Sample Detail");
    lv_obj_set_style_text_color(titleLabel, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_text_font(titleLabel, &lv_font_montserrat_22, LV_PART_MAIN);
    lv_obj_align(titleLabel, LV_ALIGN_TOP_MID, 0, 10);

    // Create info label
    infoLabel_ = lv_label_create(root_);
    lv_obj_set_style_text_color(infoLabel_, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_text_font(infoLabel_, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_align(infoLabel_, LV_ALIGN_CENTER, 0, 0);
    
    updateInfo();
}

void UISampleDetail::onExit() {
    if (root_) {
        lv_obj_del(root_);
        root_ = nullptr;
        infoLabel_ = nullptr;
    }
}

void UISampleDetail::onInput(const InputEvent& evt) {
    switch (evt.type) {
        case InputType::ButtonPress:
        case InputType::EncoderClick:
            UINavigator::instance().pop();
            break;
        default:
            break;
    }
}

std::array<Softkey, NUM_SOFTKEYS> UISampleDetail::getSoftkeys() {
    std::array<Softkey, NUM_SOFTKEYS> keys{};
    
    // Back button
    keys[0] = {"Back", []() { UINavigator::instance().pop(); }};
    
    // Play/Stop button
    keys[1] = {isPlaying_ ? "Stop" : "Play", [this]() { togglePlayback(); }};
    
    return keys;
}

void UISampleDetail::updateInfo() {
    if (!infoLabel_) return;
    
    char infoText[256];
    snprintf(infoText, sizeof(infoText), 
        "File: %s\n\n"
        "Format: WAV\n"
        "Sample Rate: 44.1 kHz\n"
        "Bit Depth: 16-bit\n"
        "Channels: Stereo\n"
        "Duration: 2:34\n\n"
        "Status: %s",
        filename_.c_str(),
        isPlaying_ ? "Playing" : "Stopped"
    );
    
    lv_label_set_text(infoLabel_, infoText);
}

void UISampleDetail::togglePlayback() {
    isPlaying_ = !isPlaying_;
    updateInfo();
    
    ESP_LOGI(TAG, "Sample playback %s: %s", 
        isPlaying_ ? "started" : "stopped", filename_.c_str());
}

std::shared_ptr<UIPage> createSampleDetailPage(const std::string& filename) {
    return std::make_shared<UISampleDetail>(filename);
}

} // namespace wavex_ui
