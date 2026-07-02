#include "ui/ui_sample_edit_page.h"

#include <esp_log.h>

#include "components/waveform_view.h"
#include "inter_mcu.h"
#include "ui/ui_navigator.h"
#include "ui/ui_sample_browser.h"

#include <algorithm>
#include <string>

namespace wavex_ui {

namespace {
constexpr uint8_t kPreviewSlot = 0;
constexpr uint32_t kPreviewLength = 48000;
constexpr uint16_t kPreviewDecim = 64;
constexpr uint32_t kPreviewStart = 0;
constexpr uint32_t kPreviewEnd = kPreviewLength;  // frames
constexpr uint16_t kPreviewPoints =
    static_cast<uint16_t>((kPreviewEnd - kPreviewStart) / kPreviewDecim + 1);
static const char* TAG = "UI_SAMPLE_EDIT";
}  // namespace

void UISampleEditPage::onEnter(lv_obj_t* parent) {
    root_ = lv_obj_create(parent);
    lv_obj_set_size(root_, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(root_, lv_color_make(0x00, 0x00, 0x00), LV_PART_MAIN);
    lv_obj_set_style_border_width(root_, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(root_, 10, LV_PART_MAIN);
    lv_obj_set_flex_flow(root_, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(root_, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    auto title = lv_label_create(root_);
    lv_label_set_text(title, "Sample Edit");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_22, LV_PART_MAIN);
    lv_obj_set_style_text_color(title, lv_color_white(), LV_PART_MAIN);

    status_label_ = lv_label_create(root_);
    lv_label_set_text(status_label_, "Loading waveform...");
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

    inter_mcu_set_wave_chunk_listener(&UISampleEditPage::waveChunkStatic, this);

    auto* state = getSampleBrowserState();
    has_sample_ = state && !state->last_load_sample_path.empty();
    if (!has_sample_) {
        refreshStatus("Load a sample in Sample Browser, then reopen Edit.");
        return;
    }

    expected_len_ = kPreviewPoints;
    preview_buffer_.assign(expected_len_, 0);

    {
        std::string msg = "Previewing ID " + std::to_string(state->last_load_sample_id) + ": " +
                          state->last_load_sample_path;
        refreshStatus(msg.c_str());
    }
    requestWaveform();
}

void UISampleEditPage::onExit() {
    inter_mcu_set_wave_chunk_listener(nullptr, nullptr);
    waveform_.reset();
    if (root_) {
        lv_obj_del(root_);
        root_ = nullptr;
    }
}

void UISampleEditPage::onInput(const InputEvent& evt) {
    switch (evt.type) {
        case InputType::ButtonPress:
        case InputType::EncoderClick:
            requestWaveform();
            break;
        default:
            break;
    }
}

std::array<Softkey, NUM_SOFTKEYS> UISampleEditPage::getSoftkeys() {
    std::array<Softkey, NUM_SOFTKEYS> keys{};
    keys[0] = {"Back", []() { UINavigator::instance().pop(); }};
    keys[1] = {"Refresh", [this]() { requestWaveform(); }};
    return keys;
}

void UISampleEditPage::waveChunkStatic(uint32_t offset,
                                       const int16_t* samples,
                                       uint16_t count,
                                       void* user) {
    if (!user)
        return;
    static_cast<UISampleEditPage*>(user)->handleWaveChunk(offset, samples, count);
}

void UISampleEditPage::handleWaveChunk(uint32_t offset, const int16_t* samples, uint16_t count) {
    if (!samples || count == 0) {
        refreshStatus("Waveform: no data");
        if (waveform_)
            waveform_->clear();
        return;
    }

    ESP_LOGI(TAG, "Wave chunk received: offset=%u count=%u", (unsigned)offset, (unsigned)count);

    if (expected_len_ == 0) {
        expected_len_ = kPreviewPoints;
        preview_buffer_.assign(expected_len_, 0);
    }

    const uint32_t needed = offset + count;
    if (preview_buffer_.size() < needed) {
        preview_buffer_.resize(needed, 0);
    }

    std::copy(samples, samples + count, preview_buffer_.begin() + offset);

    const uint32_t filled =
        std::min<uint32_t>(static_cast<uint32_t>(preview_buffer_.size()), expected_len_);
    if (waveform_ && filled > 0) {
        waveform_->setSamples(preview_buffer_.data(), static_cast<uint16_t>(filled));
    }
    refreshStatus("Waveform updated");
}

void UISampleEditPage::requestWaveform() {
    if (!has_sample_) {
        refreshStatus("No sample loaded. Load via Sample Browser first.");
        return;
    }
    expected_len_ = kPreviewPoints;
    preview_buffer_.assign(expected_len_, 0);
    esp_err_t res = inter_mcu_send_preview_req(kPreviewSlot, 0, kPreviewLength, kPreviewDecim);
    if (res != ESP_OK) {
        refreshStatus("Waveform request failed");
    } else {
        refreshStatus("Requesting waveform...");
    }
}

void UISampleEditPage::refreshStatus(const char* text) {
    if (status_label_) {
        lv_label_set_text(status_label_, text);
    }
}

std::shared_ptr<UIPage> createSampleEditPage() {
    return std::make_shared<UISampleEditPage>();
}

}  // namespace wavex_ui
