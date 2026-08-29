#include "ui/ui_sample_edit_page.h"

#include <esp_log.h>

#include "components/waveform_view.h"
#include "inter_mcu.h"
#include "ui/ui_navigator.h"
#include "ui/ui_sample_browser.h"

#include <algorithm>
#include <cstdio>
#include <string>

namespace wavex_ui {

namespace {

constexpr uint8_t kPreviewSlot = 0;
// The backend previews from a fixed slot; until it reports real sample length
// we treat this window as the sample. Zoom narrows it, it never grows past it.
constexpr uint32_t kSampleFrames = 48000;
constexpr uint16_t kPreviewPoints = 512;
constexpr uint32_t kMinWindow = 512;

// Layout, page-relative (the navigator's content area already starts below the
// 75px header). Design 2e: waveform 1256x250 @ y12, param strip y278, info y434.
constexpr int kMargin = 12;
constexpr int kWaveW = 1256;
constexpr int kWaveY = 12;
constexpr int kWaveH = 250;
constexpr int kStripY = 278;
constexpr int kCardW = 305;
constexpr int kCardH = 132;
constexpr int kCardPitch = 317;
constexpr int kGaugeW = 273;
constexpr int kInfoY = 434;
constexpr int kInfoH = 88;

constexpr uint32_t kColCard = 0x141414;
constexpr uint32_t kColBorder = 0x2A2A2A;
constexpr uint32_t kColDim = 0x8FA0AA;
constexpr uint32_t kColDimmer = 0x5A6670;
constexpr uint32_t kColBlue = 0x2196F3;
constexpr uint32_t kColGreen = 0x4CAF50;
constexpr uint32_t kColOrange = 0xFF9800;

const char* TAG = "UI_SAMPLE_EDIT";

lv_obj_t* box(lv_obj_t* parent, int x, int y, int w, int h, uint32_t colour) {
    lv_obj_t* o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_size(o, w, h);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_style_bg_color(o, lv_color_hex(colour), 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    return o;
}

lv_obj_t* label(
    lv_obj_t* parent, int x, int y, const char* text, const lv_font_t* font, uint32_t colour) {
    lv_obj_t* l = lv_label_create(parent);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(colour), 0);
    lv_obj_set_pos(l, x, y);
    return l;
}

// mm:ss.mmm from a frame count, assuming the preview window's own rate. The
// backend does not send the sample rate with a wave chunk, so this is frames
// expressed at 48k rather than a measured time - said plainly in the label.
void formatFrames(char* out, size_t n, uint32_t frames) {
    const uint32_t ms = static_cast<uint32_t>((frames * 1000ULL) / 48000ULL);
    snprintf(out,
             n,
             "%lu:%02lu.%03lu",
             static_cast<unsigned long>(ms / 60000),
             static_cast<unsigned long>((ms / 1000) % 60),
             static_cast<unsigned long>(ms % 1000));
}

}  // namespace

void UISampleEditPage::onEnter(lv_obj_t* parent) {
    root_ = lv_obj_create(parent);
    lv_obj_remove_style_all(root_);
    lv_obj_set_size(root_, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(root_, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(root_, LV_OPA_COVER, 0);
    lv_obj_remove_flag(root_, LV_OBJ_FLAG_SCROLLABLE);

    buildWaveformPanel(root_);
    buildParamStrip(root_);
    buildInfoStrip(root_);

    inter_mcu_set_wave_chunk_listener(&UISampleEditPage::waveChunkStatic, this);

    auto* state = getSampleBrowserState();
    has_sample_ = state && !state->last_load_sample_path.empty();
    if (!has_sample_) {
        refreshStatus("Load a sample in Sample Browser, then reopen Edit.");
        refreshParams();
        return;
    }

    window_frames_ = kSampleFrames;
    start_frame_ = 0;
    end_frame_ = window_frames_;
    expected_len_ = kPreviewPoints;
    preview_buffer_.assign(expected_len_, 0);

    refreshStatus(state->last_load_sample_path.c_str());
    refreshParams();
    requestWaveform();
}

void UISampleEditPage::buildWaveformPanel(lv_obj_t* parent) {
    lv_obj_t* panel = box(parent, kMargin, kWaveY, kWaveW, kWaveH, 0x101010);
    lv_obj_set_style_border_width(panel, 1, 0);
    lv_obj_set_style_border_color(panel, lv_color_hex(kColBorder), 0);
    lv_obj_set_style_pad_all(panel, 4, 0);

    waveform_ = std::make_unique<WaveformView>(panel, lv_pct(100), lv_pct(100));

    // Region handles: 30x26 tabs on the top edge, S green / E orange, matching
    // the marker colours the browser's preview already uses.
    marker_s_ = box(parent, kMargin, kWaveY, 30, 26, kColGreen);
    lv_obj_t* ls = label(marker_s_, 0, 0, "S", &lv_font_montserrat_18, 0x0A0A0A);
    lv_obj_center(ls);
    marker_e_ = box(parent, kMargin + kWaveW - 30, kWaveY, 30, 26, kColOrange);
    lv_obj_t* le = label(marker_e_, 0, 0, "E", &lv_font_montserrat_18, 0x0A0A0A);
    lv_obj_center(le);
}

void UISampleEditPage::buildParamStrip(lv_obj_t* parent) {
    static const char* titles[4] = {"START", "END", "GAIN", "LOOP"};
    for (int i = 0; i < 4; i++) {
        const bool live = (i < PARAM_COUNT);
        lv_obj_t* c = box(parent, kMargin + i * kCardPitch, kStripY, kCardW, kCardH, kColCard);
        lv_obj_set_style_border_width(c, 1, 0);
        lv_obj_set_style_border_color(c, lv_color_hex(kColBorder), 0);
        lv_obj_set_style_radius(c, 4, 0);

        label(c, 16, 12, titles[i], &lv_font_montserrat_18, live ? kColDim : kColDimmer);

        if (!live) {
            // Drawn so the strip reads as the design intends, but explicitly
            // inert: no protocol message carries a per-sample gain or loop
            // point, so a control here would do nothing.
            label(c, 16, 44, "--", &lv_font_montserrat_32, kColDimmer);
            label(c, 16, 96, "needs protocol", &lv_font_montserrat_14, kColDimmer);
            continue;
        }

        cards_[i].card = c;
        cards_[i].value = label(c, 16, 40, "0:00.000", &lv_font_montserrat_32, 0xFFFFFF);
        box(c, 16, 96, kGaugeW, 14, 0x1F1F1F);
        cards_[i].bar = box(c, 16, 96, 0, 14, kColBlue);
        cards_[i].knob = box(c, 12, 92, 8, 22, 0xFFFFFF);
    }
    refreshFocusRing();
}

void UISampleEditPage::buildInfoStrip(lv_obj_t* parent) {
    lv_obj_t* info = box(parent, kMargin, kInfoY, kWaveW, kInfoH, kColCard);
    lv_obj_set_style_border_width(info, 1, 0);
    lv_obj_set_style_border_color(info, lv_color_hex(kColBorder), 0);
    lv_obj_set_style_radius(info, 4, 0);

    info_label_ = label(info, 16, 14, "", &lv_font_montserrat_18, kColDim);
    status_label_ = label(info, 16, 48, "", &lv_font_montserrat_18, kColGreen);
    lv_obj_set_width(status_label_, kWaveW - 32);
    lv_label_set_long_mode(status_label_, LV_LABEL_LONG_DOT);
}

void UISampleEditPage::onExit() {
    inter_mcu_set_wave_chunk_listener(nullptr, nullptr);
    waveform_.reset();
    if (root_) {
        lv_obj_del(root_);
        root_ = nullptr;
    }
    status_label_ = nullptr;
    info_label_ = nullptr;
    marker_s_ = nullptr;
    marker_e_ = nullptr;
    for (auto& c: cards_) {
        c = ParamCard{};
    }
}

void UISampleEditPage::onInput(const InputEvent& evt) {
    switch (evt.type) {
        case InputType::EncoderRight:
        case InputType::EncoderUp:
            adjustFocused(evt.delta ? evt.delta : 1);
            break;
        case InputType::EncoderLeft:
        case InputType::EncoderDown:
            adjustFocused(evt.delta ? -evt.delta : -1);
            break;
        case InputType::EncoderClick:
            focus_ = static_cast<uint8_t>((focus_ + 1) % PARAM_COUNT);
            refreshFocusRing();
            break;
        default:
            break;
    }
}

std::array<Softkey, NUM_SOFTKEYS> UISampleEditPage::getSoftkeys() {
    std::array<Softkey, NUM_SOFTKEYS> keys{};
    keys[0] = {"Back", []() { UINavigator::instance().pop(); }};
    // Audition replays the whole file: MSG_SAMPLE_PLAY_INDEX_REQ carries no
    // range, so the region below is not honoured by playback yet.
    keys[1] = {"Audition", [this]() {
                   auto* state = getSampleBrowserState();
                   if (!state) {
                       return;
                   }
                   if (inter_mcu_send_sample_play_index_req(state->selected_file_index) == ESP_OK) {
                       refreshStatus("Auditioning whole file (range not sent - no protocol)");
                   } else {
                       refreshStatus("Audition request failed");
                   }
               }};
    keys[2] = {"Zoom -", [this]() { setZoom(-1); }};
    keys[3] = {"Zoom +", [this]() { setZoom(1); }};
    keys[4] = {"Param >", [this]() {
                   focus_ = static_cast<uint8_t>((focus_ + 1) % PARAM_COUNT);
                   refreshFocusRing();
               }};
    keys[5] = {"Refresh", [this]() { requestWaveform(); }};
    return keys;
}

void UISampleEditPage::adjustFocused(int steps) {
    if (!has_sample_ || window_frames_ == 0) {
        return;
    }
    // One detent moves 1/200th of the visible window, so the same gesture is
    // coarse when zoomed out and fine when zoomed in.
    const int32_t step = static_cast<int32_t>(window_frames_ / 200) + 1;
    const int64_t delta = static_cast<int64_t>(steps) * step;

    if (focus_ == PARAM_START) {
        int64_t v = static_cast<int64_t>(start_frame_) + delta;
        v = std::max<int64_t>(0, std::min<int64_t>(v, static_cast<int64_t>(end_frame_) - 1));
        start_frame_ = static_cast<uint32_t>(v);
    } else {
        int64_t v = static_cast<int64_t>(end_frame_) + delta;
        v = std::max<int64_t>(static_cast<int64_t>(start_frame_) + 1,
                              std::min<int64_t>(v, static_cast<int64_t>(kSampleFrames)));
        end_frame_ = static_cast<uint32_t>(v);
    }
    refreshParams();
    requestWaveform();
}

void UISampleEditPage::setZoom(int direction) {
    if (!has_sample_) {
        return;
    }
    if (direction > 0) {
        window_frames_ = std::max(kMinWindow, window_frames_ / 2);
    } else {
        window_frames_ = std::min(kSampleFrames, window_frames_ * 2);
    }
    // Keep the region inside the window so zooming never strands the markers
    // off-screen with no way back.
    if (end_frame_ > window_frames_) {
        end_frame_ = window_frames_;
    }
    if (start_frame_ >= end_frame_) {
        start_frame_ = end_frame_ > 0 ? end_frame_ - 1 : 0;
    }
    refreshParams();
    requestWaveform();
}

void UISampleEditPage::refreshParams() {
    char buf[32];
    const uint32_t span = window_frames_ ? window_frames_ : 1;

    if (cards_[PARAM_START].value) {
        formatFrames(buf, sizeof(buf), start_frame_);
        lv_label_set_text(cards_[PARAM_START].value, buf);
        const int pct = static_cast<int>((start_frame_ * 100ULL) / span);
        lv_obj_set_width(cards_[PARAM_START].bar, (kGaugeW * pct) / 100);
        lv_obj_set_x(cards_[PARAM_START].knob, 16 + (kGaugeW * pct) / 100 - 4);
    }
    if (cards_[PARAM_END].value) {
        formatFrames(buf, sizeof(buf), end_frame_);
        lv_label_set_text(cards_[PARAM_END].value, buf);
        const int pct = static_cast<int>(std::min<uint64_t>(100, (end_frame_ * 100ULL) / span));
        lv_obj_set_width(cards_[PARAM_END].bar, (kGaugeW * pct) / 100);
        lv_obj_set_x(cards_[PARAM_END].knob, 16 + (kGaugeW * pct) / 100 - 4);
    }

    // Marker tabs track the region across the waveform panel.
    if (marker_s_ && marker_e_) {
        const int sx = static_cast<int>((static_cast<uint64_t>(start_frame_) * kWaveW) / span);
        const int ex = static_cast<int>(
            std::min<uint64_t>(kWaveW, (static_cast<uint64_t>(end_frame_) * kWaveW) / span));
        lv_obj_set_x(marker_s_, kMargin + std::min(sx, kWaveW - 30));
        lv_obj_set_x(marker_e_, kMargin + std::max(0, ex - 30));
    }

    if (info_label_) {
        char a[32], b[32];
        formatFrames(a, sizeof(a), end_frame_ - start_frame_);
        formatFrames(b, sizeof(b), window_frames_);
        snprintf(buf, sizeof(buf), "%lu", static_cast<unsigned long>(end_frame_ - start_frame_));
        char line[160];
        snprintf(line,
                 sizeof(line),
                 "selection %s (%s frames at 48k)  -  window %s  -  zoom 1:%lu",
                 a,
                 buf,
                 b,
                 static_cast<unsigned long>(kSampleFrames / (window_frames_ ? window_frames_ : 1)));
        lv_label_set_text(info_label_, line);
    }
}

void UISampleEditPage::refreshFocusRing() {
    for (uint8_t i = 0; i < PARAM_COUNT; i++) {
        if (!cards_[i].card) {
            continue;
        }
        const bool on = (i == focus_);
        lv_obj_set_style_border_width(cards_[i].card, on ? 2 : 1, 0);
        lv_obj_set_style_border_color(cards_[i].card, lv_color_hex(on ? kColBlue : kColBorder), 0);
    }
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
}

void UISampleEditPage::requestWaveform() {
    if (!has_sample_) {
        refreshStatus("No sample loaded. Load via Sample Browser first.");
        return;
    }
    // Decimate so a full window always arrives as roughly kPreviewPoints,
    // whatever the zoom - otherwise zooming in would fetch the same number of
    // frames and show the same detail.
    const uint32_t span = window_frames_ ? window_frames_ : kSampleFrames;
    uint32_t decim = span / kPreviewPoints;
    if (decim == 0) {
        decim = 1;
    }
    expected_len_ = span / decim;
    preview_buffer_.assign(expected_len_, 0);

    const esp_err_t res =
        inter_mcu_send_preview_req(kPreviewSlot, 0, span, static_cast<uint16_t>(decim));
    if (res != ESP_OK) {
        refreshStatus("Waveform request failed");
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
