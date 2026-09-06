#include "ui/ui_sample_record_page.h"

#include <esp_timer.h>

#include "components/envelope_cache.h"
#include "components/envelope_panel.h"
#include "components/waveform_view.h"
#include "inter_mcu.h"
#include "ui/current_sample.h"
#include "ui/ui_navigator.h"
#include "ui_theme.h"

namespace wavex_ui {

namespace {

// Same request policy as the Edit tab: a run the backend drops is retried a
// few times, then the page says so rather than polling forever.
constexpr uint32_t kRequestTimeoutMs = 3000;
constexpr uint8_t kMaxRequestRetries = 3;

// Layout, page-relative, matching the Edit tab's waveform panel so the trace
// sits in the same place when the user switches between them.
constexpr int kMargin = UI_MARGIN_X;
constexpr int kWaveW = UI_SCREEN_WIDTH - 2 * kMargin;  // 1240
constexpr int kWaveY = 12;
constexpr int kWaveH = 220;
constexpr int kWavePad = 4;
constexpr int kWaveBorder = 1;
// The panel's content area: inside its border and padding. LVGL sizes
// children against this, not the panel's outer box, and WaveformView takes
// its column count from the width it is given.
constexpr int kWaveInnerW = kWaveW - 2 * (kWavePad + kWaveBorder);  // 1230
constexpr int kWaveInnerH = kWaveH - 2 * (kWavePad + kWaveBorder);  // 210
constexpr int kNameY = kWaveY + kWaveH + 16;                        // 248
constexpr int kStatusY = kNameY + 36;                               // 284

}  // namespace

void UISampleRecordPage::onEnter(lv_obj_t* parent) {
    root_ = lv_obj_create(parent);
    lv_obj_remove_style_all(root_);
    lv_obj_set_size(root_, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(root_, UI_COLOR_BG, 0);
    lv_obj_set_style_bg_opa(root_, LV_OPA_COVER, 0);
    lv_obj_remove_flag(root_, LV_OBJ_FLAG_SCROLLABLE);

    EnsureEnvelopeCacheInitialised();

    lv_obj_t* panel = lv_obj_create(root_);
    lv_obj_remove_style_all(panel);
    lv_obj_set_size(panel, kWaveW, kWaveH);
    lv_obj_set_pos(panel, kMargin, kWaveY);
    lv_obj_set_style_bg_color(panel, UI_COLOR_CARD, 0);
    lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(panel, kWaveBorder, 0);
    lv_obj_set_style_border_color(panel, UI_COLOR_LINE, 0);
    lv_obj_set_style_pad_all(panel, kWavePad, 0);
    lv_obj_remove_flag(panel, LV_OBJ_FLAG_SCROLLABLE);

    waveform_ = std::make_unique<WaveformView>(panel, kWaveInnerW, kWaveInnerH);

    name_label_ = lv_label_create(root_);
    lv_label_set_text(name_label_, "");
    lv_obj_set_style_text_font(name_label_, UI_FONT_TITLE, 0);
    lv_obj_set_style_text_color(name_label_, UI_COLOR_TEXT, 0);
    lv_obj_set_pos(name_label_, kMargin, kNameY);

    status_label_ = lv_label_create(root_);
    lv_label_set_text(status_label_, "");
    lv_obj_set_style_text_font(status_label_, UI_FONT_SMALL, 0);
    lv_obj_set_style_text_color(status_label_, UI_COLOR_DIM, 0);
    lv_obj_set_pos(status_label_, kMargin, kStatusY);

    EnvelopePanel::Config cfg;
    cfg.timeout_ms = kRequestTimeoutMs;
    cfg.max_retries = kMaxRequestRetries;
    EnvelopeSink* sinks[] = {waveform_.get()};
    panel_.attach(cfg, EspEnvelopeLink(), &GetEnvelopeCache(), sinks, 1);
    shown_sample_id_ = 0;
    shown_generation_ = 0;
    no_sample_shown_ = false;
    wave_status_shown_ = false;

    ui_timer_ = lv_timer_create(&UISampleRecordPage::uiTimerCb, 50, this);
    syncSample();
}

void UISampleRecordPage::onExit() {
    // The panel releases the chunk listener and any run in flight before the
    // view it would have drawn into is destroyed.
    panel_.detach();
    if (ui_timer_) {
        lv_timer_delete(ui_timer_);
        ui_timer_ = nullptr;
    }
    waveform_.reset();
    if (root_) {
        lv_obj_del(root_);
        root_ = nullptr;
    }
    name_label_ = nullptr;
    status_label_ = nullptr;
}

std::array<Softkey, NUM_SOFTKEYS> UISampleRecordPage::getSoftkeys() {
    std::array<Softkey, NUM_SOFTKEYS> keys{};
    keys[0] = {"Back", []() { UINavigator::instance().pop(); }};
    // Dimmed, not hidden: the row is six fixed positions. It stays until the
    // backend can capture (roadmap Phase 1), so the key that will do it is
    // already where it will be.
    keys[1] = {"Record", nullptr, false, "recording not implemented"};
    // Ask again, from scratch: after a give-up this is the only way to retry
    // without leaving the tab.
    keys[2] = {"Refresh", [this]() {
                   shown_sample_id_ = 0;
                   shown_generation_ = 0;
               }};
    return keys;
}

void UISampleRecordPage::uiTimerCb(lv_timer_t* timer) {
    auto* self = static_cast<UISampleRecordPage*>(lv_timer_get_user_data(timer));
    if (self) {
        self->serviceUi();
    }
}

// UI task, inside an lv_timer. Follows the current sample and drives the
// panel; the panel coalesces and retries on its own, this only words it.
void UISampleRecordPage::serviceUi() {
    const uint32_t now = static_cast<uint32_t>(esp_timer_get_time() / 1000);
    syncSample();
    switch (panel_.service(now)) {
        case EnvelopePanel::Event::Drawn:
            if (wave_status_shown_) {
                wave_status_shown_ = false;
                setStatus("Recording is not implemented yet. Showing the current sample.");
            }
            break;
        case EnvelopePanel::Event::Retrying:
            wave_status_shown_ = true;
            setStatus("Waveform request timed out - retrying");
            break;
        case EnvelopePanel::Event::SendFailed:
            wave_status_shown_ = true;
            setStatus("Waveform request refused by the link - retrying");
            break;
        case EnvelopePanel::Event::GaveUp:
            wave_status_shown_ = true;
            setStatus("Waveform unavailable - Refresh to retry");
            break;
        case EnvelopePanel::Event::None:
            break;
    }
}

// Hands the panel the current sample whenever it, or its generation, differs
// from what was last shown. Geometry comes from the backend's cached metadata
// so any resident sample shows, however it got there; a sample whose metadata
// has not arrived yet is simply tried again next tick.
void UISampleRecordPage::syncSample() {
    if (!panel_.attached()) {
        return;
    }
    const uint16_t sample_id = getCurrentSampleId();
    if (sample_id == 0) {
        if (panel_.hasSample()) {
            panel_.clearSample();
        }
        if (shown_sample_id_ != 0 || !no_sample_shown_) {
            shown_sample_id_ = 0;
            shown_generation_ = 0;
            no_sample_shown_ = true;
            wave_status_shown_ = false;
            lv_label_set_text(name_label_, "No sample selected");
            setStatus("Recording is not implemented yet. Load a sample to see it here.");
        }
        return;
    }

    WaveX::Protocol::SampleMetadata m;
    if (!inter_mcu_get_sample_meta(sample_id, &m) || m.total_frames == 0) {
        return;
    }
    if (sample_id == shown_sample_id_ && m.generation == shown_generation_) {
        return;
    }
    shown_sample_id_ = sample_id;
    shown_generation_ = m.generation;
    no_sample_shown_ = false;
    lv_label_set_text(name_label_, m.name);
    panel_.setWindow(0, 0, m.total_frames);
    panel_.setSample(sample_id, m.generation, m.total_frames);
    // Worded on Drawn, so the line reads as an outcome and not a promise.
    wave_status_shown_ = true;
    setStatus("Loading waveform...");
}

void UISampleRecordPage::setStatus(const char* text) {
    if (status_label_ && text) {
        lv_label_set_text(status_label_, text);
    }
}

std::shared_ptr<UIPage> createSampleRecordPage() {
    return std::make_shared<UISampleRecordPage>();
}

}  // namespace wavex_ui
