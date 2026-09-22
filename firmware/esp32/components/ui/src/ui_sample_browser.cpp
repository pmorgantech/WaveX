// WaveX UI Sample Browser Implementation
#include "ui/ui_sample_browser.h"

#include <esp_log.h>
#include <esp_timer.h>
#include <strings.h>

#include "../components/envelope_cache.h"
#include "../components/envelope_panel.h"
#include "../components/waveform_view.h"
#include "../styles/ui_theme.h"
#include "comm/i_comm_interface.h"
#include "debug/console_command.h"
#include "esp_lvgl_port.h"
#include "inter_mcu.h"
#include "ui/current_sample.h"
#include "ui/current_track.h"
#include "ui/sample_load_failure.h"
#include "ui/ui_api.h"
#include "ui/ui_busy_overlay.h"
#include "ui/ui_palette.h"
#include "ui_task.h"

#include "wav/resident_sample_policy.hpp"
#include <algorithm>

static const char* TAG = "UI_SAMPLE_BROWSER";

namespace wavex_ui {

namespace {
// Design 1b, page-relative (the content area already starts below the header).
constexpr int kMargin = 12;
constexpr int kStatusY = 12;
constexpr int kListY = 46;
constexpr int kListW = 770;
constexpr int kListH = 487;
constexpr int kDetailX = 794;
constexpr int kDetailY = 12;
constexpr int kDetailW = 474;
constexpr int kDetailH = 521;

// Local names for the shared palette (ui/ui_palette.h). These were
// hand-copied literals that had already drifted from it and from each
// other - three different "border" greys existed across five files - so a
// theme switch reached only the surfaces that happened to be in sync.
constexpr uint32_t kColBg = palette::kColBg;
constexpr uint32_t kColPanel = palette::kColCard;
constexpr uint32_t kColBorder = palette::kColBorder;
constexpr uint32_t kColDim = palette::kColDim;
constexpr uint32_t kColGreen = palette::kColGreen;

// One line of UI_FONT_TITLE. The headline is clipped to this so it cannot
// wrap into the waveform panel.
constexpr int kHeadlineH = 32;
// Status line above the play bar: two lines of UI_FONT_SMALL.
constexpr int kDetailStatusY = 444;
constexpr int kDetailStatusH = 44;

// Waveform preview, in the gap the design leaves between the filename headline
// and the metadata rows.
constexpr int kWaveX = 16;
constexpr int kWaveY = 58;
constexpr int kWaveW = kDetailW - 32;
constexpr int kWaveH = 150;
// Inside the panel's 1 px border. The view is built at this size in pixels
// so it knows its column count - one per pixel - and asks for exactly that.
constexpr int kWaveInnerW = kWaveW - 2;
constexpr int kWaveInnerH = kWaveH - 2;

constexpr uint32_t kWaveTimeoutMs = 3000;
constexpr uint8_t kWaveMaxRetries = 2;

bool isInstrumentFile(const char* name) {
    return WaveX::Protocol::IsInstrumentFileName(name);
}

void formatBytes(uint32_t bytes, char* out, size_t out_size) {
    if (bytes < 1024u) {
        snprintf(out, out_size, "%lu B", (unsigned long)bytes);
    } else if (bytes < 1024u * 1024u) {
        snprintf(out, out_size, "%.1f KB", static_cast<double>(bytes) / 1024.0);
    } else {
        snprintf(out, out_size, "%.1f MB", static_cast<double>(bytes) / (1024.0 * 1024.0));
    }
}

}  // namespace

// Active instance for callbacks
UISampleBrowser* UISampleBrowser::s_active_instance_ = nullptr;

UISampleBrowser::UISampleBrowser(WaveX::Comm::ICommInterface& comm_interface,
                                 SampleBrowserState& persistent_state,
                                 bool instruments)
    : comm_interface_(&comm_interface),
      persistent_state_(persistent_state),
      instruments_(instruments) {}

UISampleBrowser::~UISampleBrowser() {
    // Cleanup is done in onExit()
}

void UISampleBrowser::onEnter(lv_obj_t* parent) {
    // NOTE: onEnter is called from UINavigator::push/pop which already holds LVGL lock
    // No need to acquire lock here

    ESP_LOGI(TAG,
             "=== SAMPLE BROWSER ON_ENTER: Starting initialization, persistent_state.is_playing=%d",
             persistent_state_.is_playing ? 1 : 0);

    root_ = lv_obj_create(parent);
    lv_obj_set_size(root_, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(root_, UI_COLOR_CONTENT, LV_PART_MAIN);
    lv_obj_set_style_border_width(root_, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(root_, 0, LV_PART_MAIN);
    lv_obj_align(root_, LV_ALIGN_TOP_LEFT, 0, 0);

    lv_obj_set_style_pad_all(root_, 0, LV_PART_MAIN);
    lv_obj_remove_flag(root_, LV_OBJ_FLAG_SCROLLABLE);

    // Status strip above the list: position in the listing on the left, card
    // state on the right. Both were previously buried in the info panel.
    listing_label_ = lv_label_create(root_);
    lv_obj_set_style_text_font(listing_label_, UI_FONT_SMALL, LV_PART_MAIN);
    lv_obj_set_style_text_color(listing_label_, lv_color_hex(kColDim), LV_PART_MAIN);
    lv_label_set_text(listing_label_, "");
    lv_obj_set_pos(listing_label_, kMargin, kStatusY);

    card_label_ = lv_label_create(root_);
    lv_obj_set_style_text_font(card_label_, UI_FONT_SMALL, LV_PART_MAIN);
    lv_obj_set_style_text_color(card_label_, lv_color_hex(kColDim), LV_PART_MAIN);
    lv_label_set_text(card_label_, "");
    lv_obj_set_pos(card_label_, 600, kStatusY);

    // File list, 770x487 (design 1b).
    browser_container_ = lv_obj_create(root_);
    lv_obj_set_size(browser_container_, kListW, kListH);
    lv_obj_set_pos(browser_container_, kMargin, kListY);
    lv_obj_set_style_bg_color(browser_container_, lv_color_hex(kColPanel), LV_PART_MAIN);
    lv_obj_set_style_border_width(browser_container_, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(browser_container_, lv_color_hex(kColBorder), LV_PART_MAIN);
    lv_obj_set_style_pad_all(browser_container_, 0, LV_PART_MAIN);

    // Detail panel, 474x521.
    info_panel_ = lv_obj_create(root_);
    lv_obj_set_size(info_panel_, kDetailW, kDetailH);
    lv_obj_set_pos(info_panel_, kDetailX, kDetailY);
    lv_obj_set_style_bg_color(info_panel_, lv_color_hex(kColPanel), LV_PART_MAIN);
    lv_obj_set_style_border_width(info_panel_, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(info_panel_, lv_color_hex(kColBorder), LV_PART_MAIN);
    lv_obj_set_style_pad_all(info_panel_, 0, LV_PART_MAIN);
    lv_obj_remove_flag(info_panel_, LV_OBJ_FLAG_SCROLLABLE);

    detail_name_ = lv_label_create(info_panel_);
    lv_obj_set_style_text_font(detail_name_, UI_FONT_TITLE, LV_PART_MAIN);
    lv_obj_set_style_text_color(detail_name_, UI_COLOR_TEXT, LV_PART_MAIN);
    lv_obj_set_pos(detail_name_, 16, 14);
    // Fixed height as well as width: LV_LABEL_LONG_DOT only truncates when
    // the label cannot grow, and with the height left to content a long name
    // wrapped to a second line straight across the waveform panel below.
    lv_obj_set_size(detail_name_, kDetailW - 32, kHeadlineH);
    lv_label_set_long_mode(detail_name_, LV_LABEL_LONG_DOT);
    lv_label_set_text(detail_name_, "Select a file");

    // Waveform preview of the loaded sample. Built unconditionally so the
    // panel's geometry does not shift when one appears; it simply draws its
    // zero line until there is something to show.
    EnsureEnvelopeCacheInitialised();

    lv_obj_t* wave_panel = lv_obj_create(info_panel_);
    lv_obj_remove_style_all(wave_panel);
    lv_obj_set_size(wave_panel, kWaveW, kWaveH);
    lv_obj_set_pos(wave_panel, kWaveX, kWaveY);
    // An inset well: one step darker than the card it sits in, from the theme
    // like everything else here (this was a fixed near-black).
    lv_obj_set_style_bg_color(wave_panel, lv_color_hex(kColBg), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(wave_panel, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(wave_panel, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(wave_panel, lv_color_hex(kColBorder), LV_PART_MAIN);
    lv_obj_remove_flag(wave_panel, LV_OBJ_FLAG_SCROLLABLE);
    waveform_ = std::make_unique<WaveformView>(wave_panel, kWaveInnerW, kWaveInnerH);

    // Says why the panel is empty. Without it an unloaded selection looks
    // identical to a preview that failed, which is the ambiguity the busy
    // overlay was criticised for elsewhere.
    waveform_hint_ = lv_label_create(info_panel_);
    lv_obj_set_style_text_font(waveform_hint_, UI_FONT_SMALL, LV_PART_MAIN);
    lv_obj_set_style_text_color(waveform_hint_, lv_color_hex(kColDim), LV_PART_MAIN);
    // On the well's own colour, so the zero line does not run through the
    // words.
    lv_obj_set_style_bg_color(waveform_hint_, lv_color_hex(kColBg), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(waveform_hint_, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_pad_hor(waveform_hint_, 6, LV_PART_MAIN);
    lv_obj_set_style_pad_ver(waveform_hint_, 2, LV_PART_MAIN);
    lv_obj_set_pos(waveform_hint_, kWaveX + 10, kWaveY + (kWaveH / 2) - 12);
    lv_label_set_text(waveform_hint_, "Load to preview");

    // One view, the whole file: this panel does not zoom, so one coarse tier
    // serves it and stays cached cheaply across selections.
    EnvelopePanel::Config wave_cfg;
    wave_cfg.timeout_ms = kWaveTimeoutMs;
    // Fewer retries than the edit page: this is a preview beside the real
    // information, not the thing the user came to look at, and a browser that
    // keeps retrying in the background is traffic nobody asked for.
    wave_cfg.max_retries = kWaveMaxRetries;
    EnvelopeSink* sinks[] = {waveform_.get()};
    envelope_panel_.attach(wave_cfg, EspEnvelopeLink(), &GetEnvelopeCache(), sinks, 1);
    shown_sample_id_ = 0;
    shown_generation_ = 0;

    // Metadata rows. Kept as one wrapped label rather than a table: the
    // fields are fixed and a table's chrome costs more than it adds here.
    metadata_label_ = lv_label_create(info_panel_);
    lv_obj_set_style_text_font(metadata_label_, UI_FONT_SMALL, LV_PART_MAIN);
    lv_obj_set_style_text_color(metadata_label_, lv_color_hex(kColDim), LV_PART_MAIN);
    lv_obj_set_pos(metadata_label_, 16, 222);
    lv_obj_set_width(metadata_label_, kDetailW - 32);
    lv_label_set_long_mode(metadata_label_, LV_LABEL_LONG_WRAP);
    lv_label_set_text(metadata_label_, "");

    status_label_ = lv_label_create(info_panel_);
    lv_obj_set_style_text_font(status_label_, UI_FONT_SMALL, LV_PART_MAIN);
    lv_obj_set_style_text_color(status_label_, lv_color_hex(kColGreen), LV_PART_MAIN);
    lv_obj_set_pos(status_label_, 16, kDetailStatusY);
    // Two lines, then dots. The Track-replace prompt names the resident file
    // and does not fit on one; unbounded, its second line ran under the play
    // bar and off the bottom of the panel.
    lv_obj_set_size(status_label_, kDetailW - 32, kDetailStatusH);
    lv_label_set_long_mode(status_label_, LV_LABEL_LONG_DOT);
    lv_label_set_text(status_label_, "Ready");

    play_bar_ = lv_bar_create(info_panel_);
    lv_obj_set_size(play_bar_, kDetailW - 32, 10);
    lv_obj_set_pos(play_bar_, 16, kDetailStatusY + kDetailStatusH + 4);
    lv_bar_set_range(play_bar_, 0, 100);
    lv_bar_set_value(play_bar_, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(play_bar_, lv_color_hex(0x1F1F1F), LV_PART_MAIN);
    lv_obj_set_style_bg_color(play_bar_, lv_color_hex(kColGreen), LV_PART_INDICATOR);

    if (instruments_) {
        tag_filter_label_ = lv_label_create(info_panel_);
        ui_theme_apply_label_style(tag_filter_label_, false);
        lv_obj_set_style_text_font(tag_filter_label_, UI_FONT_SMALL, 0);
        lv_obj_set_pos(tag_filter_label_, 16, kDetailStatusY - 30);
        lv_obj_set_width(tag_filter_label_, kDetailW - 32);
        lv_label_set_text_fmt(tag_filter_label_,
                              "Filter: %s / Shift to change",
                              persistent_state_.tag_filter
                                  ? WaveX::InstrumentTags::kNames[persistent_state_.tag_filter - 1]
                                  : "All");
    }
    wavex_file_browser_config_t browser_config = {.root_path = persistent_state_.current_directory_path.c_str(), .filter = instruments_ ? static_cast<WaveX::Protocol::BrowseFilter>(2 + persistent_state_.tag_filter) : WaveX::Protocol::BrowseFilter::Samples, .max_entries = WaveX::Protocol::BROWSE_DIRECTORY_ENTRY_LIMIT, .show_hidden = false, .comm_interface = comm_interface_};

    ESP_LOGI(TAG, "Creating file browser with root_path: %s", browser_config.root_path);

    file_browser_ = wavex_file_browser_create(browser_container_, &browser_config);
    if (!file_browser_) {
        ESP_LOGE(TAG, "Failed to create file browser");
        return;
    }

    // Refresh display-only allocator diagnostics; the backend owns admission.
    inter_mcu_request_sample_mem_status();

    wavex_file_browser_set_file_selected_callback(file_browser_, file_selected_callback, this);
    wavex_file_browser_set_file_selected_index_callback(
        file_browser_, file_selected_index_callback, this);
    wavex_file_browser_set_directory_changed_callback(
        file_browser_, directory_changed_callback, this);

    // Selection metadata below needs live widgets and may start an SFZ probe.
    is_initialized_ = true;
    inter_mcu_set_sample_status_listener(sample_status_callback, this);
    inter_mcu_set_inst_status_listener(instrument_status_callback, this);
    s_active_instance_ = this;
    // Load decides whether to ask before binding from the Track's state
    // (beginSampleLoad), and the picker names what each Track it is moved
    // to holds; fetch every binding now so those answers are ready. The
    // Daisy also pushes a binding whenever one changes, so the cache stays
    // right between visits.
    inter_mcu_request_track_binding(0xFF);

    is_playing_ = persistent_state_.is_playing;
    selected_file_index_ = persistent_state_.selected_file_index;
    current_directory_ = persistent_state_.current_directory_path;
    memset(selected_file_path_, 0, sizeof(selected_file_path_));

    ESP_LOGI(TAG,
             "=== STATE RESTORED: is_playing=%d, playing_path='%s', selected_index=%d",
             is_playing_ ? 1 : 0,
             persistent_state_.playing_sample_path.c_str(),
             selected_file_index_);

    if (file_browser_ && wavex_file_browser_get_entry_count(file_browser_) > 0) {
        wavex_file_browser_set_selection(file_browser_, persistent_state_.selected_file_index);

        const wavex_file_entry_t* selected_entry = wavex_file_browser_get_selected(file_browser_);
        if (selected_entry) {
            updateMetadata(selected_entry);
            selected_file_index_ = persistent_state_.selected_file_index;
            strncpy(selected_file_path_, selected_entry->path, sizeof(selected_file_path_) - 1);
            selected_file_path_[sizeof(selected_file_path_) - 1] = '\0';
        }
    }

    ESP_LOGI(TAG, "=== SAMPLE BROWSER ON_ENTER: Registering callback and setting active instance");
    is_initialized_ = true;

    ESP_LOGI(TAG,
             "=== SAMPLE BROWSER ON_ENTER: Initialization complete, is_playing=%d",
             is_playing_ ? 1 : 0);
}

void UISampleBrowser::onExit() {
    tag_filter_label_ = nullptr;
    // NOTE: onExit is called from UINavigator::push/pop which already holds LVGL lock
    // No need to acquire lock here

    ESP_LOGI(
        TAG, "=== SAMPLE BROWSER ON_EXIT: Starting cleanup, is_playing=%d", is_playing_ ? 1 : 0);

    // Immediately unregister callbacks and clear active instance to prevent any
    // callbacks from firing during cleanup or page transitions
    ESP_LOGI(TAG,
             "=== SAMPLE BROWSER ON_EXIT: Unregistering callback and clearing active instance");
    inter_mcu_set_sample_status_listener(nullptr, nullptr);
    inter_mcu_set_inst_status_listener(nullptr, nullptr);
    sample_replies_.Clear();
    instrument_replies_.Clear();
    // A half-answered picker does not survive leaving the page, and a load
    // whose completion we will no longer hear must not bind later either.
    awaiting_track_ = false;
    pending_load_ = PendingLoad::None;
    bind_on_load_track_.store(-1, std::memory_order_release);
    persistent_state_.cancelLoad();
    // Detach before the widgets go: it drops the chunk listener first, so
    // the RX task cannot write through a freed page, and releases the cache's
    // run in flight, which would otherwise block every later waveform in the
    // process, not just this page's.
    envelope_panel_.detach();
    waveform_.reset();
    waveform_hint_ = nullptr;
    shown_sample_id_ = 0;
    shown_generation_ = 0;
    if (s_active_instance_ == this) {
        s_active_instance_ = nullptr;
    }

    // Mark as not initialized to prevent any stray callbacks
    is_initialized_ = false;

    // Drop anything still queued for the UI task. These flags outlive a single
    // visit - the navigator keeps the page object on its stack and calls
    // onEnter() again on the same instance - so an update left raised here was
    // replayed on re-entry against the previous visit's freed widgets and
    // entry array.
    metadata_update_pending_.store(false, std::memory_order_relaxed);
    pending_metadata_entry_valid_ = false;
    pending_metadata_text_[0] = '\0';
    status_update_pending_.store(false, std::memory_order_relaxed);
    selection_metadata_pending_.store(false, std::memory_order_relaxed);
    play_bar_update_pending_.store(false, std::memory_order_relaxed);
    softkey_refresh_pending_.store(false, std::memory_order_relaxed);

    // NOTE: Do NOT stop playback when navigating away - allow playback to continue
    // This allows users to browse other pages while audio plays in the background

    if (file_browser_) {
        const char* current_path = wavex_file_browser_get_current_path(file_browser_);
        if (current_path) {
            persistent_state_.current_directory_path = current_path;
        }
        persistent_state_.selected_file_index =
            wavex_file_browser_get_selected_index(file_browser_);
        // Note: We keep the is_playing state as-is since playback continues across page navigation

        wavex_file_browser_destroy(file_browser_);
        file_browser_ = nullptr;
    }

    if (s_active_instance_ == this) {
        s_active_instance_ = nullptr;
    }

    if (root_) {
        lv_obj_del(root_);
        root_ = nullptr;
        browser_container_ = nullptr;
        info_panel_ = nullptr;
        status_label_ = nullptr;
        metadata_label_ = nullptr;
    }

    ESP_LOGI(TAG, "Sample Browser page destroyed");
}

void UISampleBrowser::onInput(const InputEvent& evt) {
    switch (evt.type) {
        case InputType::EncoderUp:
        case InputType::EncoderDown: {
            if (!file_browser_)
                break;

            ESP_LOGD(TAG,
                     "Received %s event for file browser scrolling (delta=%d)",
                     (evt.type == InputType::EncoderUp) ? "EncoderUp" : "EncoderDown",
                     evt.delta);

            uint32_t current_index = wavex_file_browser_get_selected_index(file_browser_);
            uint32_t entry_count = wavex_file_browser_get_entry_count(file_browser_);

            if (entry_count == 0) {
                ESP_LOGW(TAG, "No entries in file browser");
                break;
            }

            // Handle each step of delta separately for responsive scrolling.
            // Clockwise (positive steps()) walks DOWN the list to the next
            // entry, like every other list on the device. This used to be
            // keyed on the event type with EncoderUp meaning "previous", which
            // only felt right because the encoder counted backwards and
            // clockwise arrived as EncoderDown. Direction is set once per
            // encoder in hardware_config.h (WAVEX_*_DIRECTION), never here.
            const int signed_steps = evt.steps();
            const int steps = (signed_steps > 0) ? signed_steps : -signed_steps;
            bool result = false;

            for (int step = 0; step < steps && step < 10; step++) {
                if (signed_steps > 0) {
                    result = wavex_file_browser_navigate_down_entry(file_browser_);
                } else {
                    result = wavex_file_browser_navigate_up_entry(file_browser_);
                }
                if (!result) {
                    ESP_LOGD(TAG, "Navigation reached boundary at step %d/%d", step, steps);
                    break;
                }
            }

            uint32_t new_index = wavex_file_browser_get_selected_index(file_browser_);
            ESP_LOGD(TAG,
                     "After nav: new_index=%u, moved=%d steps",
                     new_index,
                     (new_index != current_index) ? 1 : 0);
            break;
        }
        case InputType::ButtonPress:
        case InputType::EncoderClick: {
            // Navigation back handled by softkeys or parent context
            break;
        }
        default:
            break;
    }
}

void UISampleBrowser::changeTagFilter(uint8_t tag) {
    if (!instruments_ || !file_browser_ || tag > 8 || awaiting_track_)
        return;
    if (wavex_file_browser_set_filter(file_browser_,
                                      static_cast<WaveX::Protocol::BrowseFilter>(2 + tag))) {
        persistent_state_.tag_filter = tag;
        if (tag_filter_label_)
            lv_label_set_text_fmt(tag_filter_label_,
                                  "Filter: %s / Shift to change",
                                  tag ? WaveX::InstrumentTags::kNames[tag - 1] : "All");
        refreshSoftkeys();
    }
}
std::array<Softkey, NUM_SOFTKEYS> UISampleBrowser::getShiftedSoftkeys() {
    auto keys = getSoftkeys();
    if (!instruments_ || awaiting_track_)
        return keys;
    const bool ready = file_browser_ && !wavex_file_browser_loading(file_browser_);
    keys[3] = {"All tags", [this] { changeTagFilter(0); }, ready, "Reading directory"};
    keys[4] = {"Tag <",
               [this] { changeTagFilter((persistent_state_.tag_filter + 8) % 9); },
               ready,
               "Reading directory"};
    keys[5] = {"Tag >",
               [this] { changeTagFilter((persistent_state_.tag_filter + 1) % 9); },
               ready,
               "Reading directory"};
    return keys;
}
std::array<Softkey, NUM_SOFTKEYS> UISampleBrowser::getSoftkeys() {
    std::array<Softkey, NUM_SOFTKEYS> keys{};
    const wavex_file_entry_t* selected =
        file_browser_ ? wavex_file_browser_get_selected(file_browser_) : nullptr;
    const bool selected_sfz =
        selected && !selected->is_directory && isInstrumentFile(selected->name);

    // Track picker: replaces the whole bar so there is no way to navigate away
    // mid-question and leave a half-answered load behind.
    if (awaiting_track_) {
        keys[0] = {"Cancel", [this]() {
                       awaiting_track_ = false;
                       pending_load_ = PendingLoad::None;
                       updateStatus("Load cancelled");
                       refreshSoftkeys();
                   }};
        keys[1] = {"Track -", [this]() {
                       target_track_ = static_cast<uint8_t>((target_track_ + WAVEX_MIX_TRACKS - 1) %
                                                            WAVEX_MIX_TRACKS);
                       refreshTrackPrompt();
                       refreshSoftkeys();
                   }};
        keys[2] = {"Track +", [this]() {
                       target_track_ = static_cast<uint8_t>((target_track_ + 1) % WAVEX_MIX_TRACKS);
                       refreshTrackPrompt();
                       refreshSoftkeys();
                   }};
        keys[3] = {"Load", [this]() {
                       awaiting_track_ = false;
                       const PendingLoad kind = pending_load_;
                       pending_load_ = PendingLoad::None;
                       // The chosen Track becomes the shared one, so Play and
                       // Sample Manager follow what just loaded.
                       setCurrentTrack(target_track_);
                       const wavex_file_entry_t* entry =
                           file_browser_ ? wavex_file_browser_get_selected(file_browser_) : nullptr;
                       const bool ok =
                           entry && (kind == PendingLoad::Instrument ? loadInstrument(entry)
                                                                     : loadSample(entry));
                       if (!ok) {
                           ESP_LOGE(TAG, "Failed to request load");
                       }
                       refreshSoftkeys();
                   }};
        // A Track that is still loading an import is not a target until it
        // has finished: the load owns the Track's refs until Commit. (A Track
        // that HOLDS an import is fine - with the Sample Pool, replacing it
        // with a sample is a per-track release behind the prompt above.)
        WaveX::Protocol::TrackBindingMessage binding;
        if (inter_mcu_get_track_binding(target_track_, &binding) &&
            binding.state == WaveX::Protocol::TRACK_BINDING_LOADING) {
            keys[3].enabled = false;
            keys[3].why = "Still loading an Instrument - wait, or pick another Track";
        }
        return keys;
    }

    keys[0] = {"Back", [this]() { UINavigator::instance().pop(); }};

    if (is_playing_) {
        keys[1] = {"Stop", [this]() {
            ESP_LOGI(TAG, "Stop audition requested");
            stopAudition(); } };
    } else {
        if (selected_sfz) {
            keys[1] = {"Audition", nullptr, false, "Load an Instrument to play it"};
        } else {
            keys[1] = {"Audition", [this]() {
            ESP_LOGI(TAG, "Audition requested");
            if (!file_browser_) {
                ESP_LOGW(TAG, "File browser not available");
                return;
            }

            const wavex_file_entry_t* selected = wavex_file_browser_get_selected(file_browser_);
            if (selected && !selected->is_directory) {
                uint32_t selected_index = wavex_file_browser_get_selected_index(file_browser_);
                auditionSampleByIndex(selected_index);
            } else {
                ESP_LOGW(TAG, "No valid file selected for audition");
            } } };
        }
    }

    // Load button - handles directory traversal
    keys[2] = {"Load", [this]() {
        ESP_LOGI(TAG, "Load button pressed");
        if (!file_browser_) {
            ESP_LOGW(TAG, "File browser not available");
            return;
        }

        const wavex_file_entry_t* selected = wavex_file_browser_get_selected(file_browser_);
        if (!selected) {
            ESP_LOGW(TAG, "No entry selected");
            return;
        }

        if (strcmp(selected->name, "..") == 0) {
            ESP_LOGI(TAG, "Navigating up to parent directory");
            wavex_file_browser_navigate_up(file_browser_);
            refreshSoftkeys();
            return;
        }

        if (selected->is_directory) {
            ESP_LOGI(TAG, "Navigating into directory: %s", selected->name);
            char normalized_path[96];
            const char* path_to_use = selected->path;

            // Remove any double slashes (but preserve root "/")
            if (path_to_use[0] == '/' && path_to_use[1] == '/') {
                normalized_path[0] = '/';
                int i = 2, j = 1;
                while (path_to_use[i] != '\0' && j < (int)sizeof(normalized_path) - 1) {
                    if (path_to_use[i] == '/' && normalized_path[j - 1] == '/') {
                        i++;
                        continue;
                    }
                    normalized_path[j++] = path_to_use[i++];
                }
                normalized_path[j] = '\0';
                path_to_use = normalized_path;
            }

            ESP_LOGI(TAG, "Normalized path: '%s' -> '%s'", selected->path, path_to_use);
            wavex_file_browser_navigate_to(file_browser_, path_to_use);
            refreshSoftkeys();
        } else {
            if (isInstrumentFile(selected->name)) {
                // Always ask which Track: the load takes that Track away from
                // Play, Instrument and Sample Manager, and a Track holding an
                // Instrument refuses a bare-sample bind afterwards.
                ESP_LOGI(TAG, "Load instrument requested for: %s", selected->name);
                askForTrack(PendingLoad::Instrument);
            } else {
                ESP_LOGI(TAG, "Load sample requested for: %s", selected->name);
                beginSampleLoad(selected);
            }
        } } };

    if (selected_sfz && (!sfz_probe_ready_ || !sfz_probe_loadable_)) {
        keys[2].enabled = false;
        keys[2].why = sfz_probe_ready_ ? "Resolve instrument warnings before loading"
                                       : "Inspecting referenced WAV files";
    }

    keys[3] = {"Up", [this]() {
        ESP_LOGI(TAG, "Up button pressed");
        if (!file_browser_) return;
        wavex_file_browser_navigate_up_entry(file_browser_); } };

    keys[4] = {"Down", [this]() {
        ESP_LOGI(TAG, "Down button pressed");
        if (!file_browser_) return;
        wavex_file_browser_navigate_down_entry(file_browser_); } };

    if (instruments_) {
        keys[1] = {"Saved", [this] {
                       if (file_browser_)
                           wavex_file_browser_navigate_to(file_browser_, "/wavex/instruments");
                   }};
        keys[5] = {"Root", [this] {
                       if (file_browser_)
                           wavex_file_browser_navigate_to(file_browser_, "/");
                   }};
    }

    return keys;
}

void UISampleBrowser::file_selected_callback(const wavex_file_entry_t* entry, void* user_data) {
    UISampleBrowser* browser = static_cast<UISampleBrowser*>(user_data);
    if (!browser || !entry)
        return;

    ESP_LOGI(TAG, "File selected: %s", entry->name);
    browser->updateMetadata(entry);
    strncpy(browser->selected_file_path_, entry->path, sizeof(browser->selected_file_path_) - 1);
    browser->selected_file_path_[sizeof(browser->selected_file_path_) - 1] = '\0';
}

void UISampleBrowser::file_selected_index_callback(uint32_t file_index,
                                                   const wavex_file_entry_t* entry,
                                                   void* user_data) {
    UISampleBrowser* browser = static_cast<UISampleBrowser*>(user_data);
    if (!browser || !entry)
        return;

    // The one INFO line the scroll path keeps: where the selection landed.
    ESP_LOGI(TAG, "File selected by index %lu: %s", (unsigned long)file_index, entry->name);
    browser->updateMetadata(entry);
    browser->selected_file_index_ = file_index;
    browser->persistent_state_.selectFile(file_index, entry->name);
    // The full path, as the load path and the backend's records have it.
    // This stored the bare NAME, so the preview compared "/dir/file.wav"
    // with "file.wav", never matched, and the detail panel's waveform never
    // appeared for a selection made by scrolling - which is every selection.
    strncpy(browser->selected_file_path_, entry->path, sizeof(browser->selected_file_path_) - 1);
    browser->selected_file_path_[sizeof(browser->selected_file_path_) - 1] = '\0';
}

void UISampleBrowser::directory_changed_callback(const char* path, void* user_data) {
    UISampleBrowser* browser = static_cast<UISampleBrowser*>(user_data);
    if (!browser || !path)
        return;

    ESP_LOGI(
        TAG, "Directory changed to: %s (current: %s)", path, browser->current_directory_.c_str());

    browser->current_directory_ = path;
    browser->persistent_state_.changeDirectory(path);

    browser->selected_file_index_ = 0;
    memset(browser->selected_file_path_, 0, sizeof(browser->selected_file_path_));
    browser->sfz_selected_ = false;
    browser->sfz_probe_ready_ = false;
    browser->sfz_probe_loadable_ = false;
    browser->sfz_sample_count_ = 0;
    browser->sfz_probe_path_[0] = '\0';
    browser->probe_request_id_.store(0, std::memory_order_release);

    strcpy(browser->pending_metadata_text_, "Select a file to view metadata");
    browser->pending_metadata_entry_valid_ = false;
    browser->metadata_update_pending_.store(true, std::memory_order_release);
    wavex_ui_mark_content_changed();

    // Show metadata for the selected file once the listing has landed. This
    // runs while applying a queued directory reply; processDeferredUpdates_()
    // reads the selection and updates the label on the UI task.
    browser->selection_metadata_pending_.store(true, std::memory_order_release);

    char status_text[256];
    if (browser->is_playing_ && !browser->persistent_state_.playing_sample_path.empty()) {
        snprintf(status_text,
                 sizeof(status_text),
                 "Playing: %s",
                 browser->persistent_state_.playing_sample_path.c_str());
        ESP_LOGI(TAG,
                 "Directory changed: Showing playing status for %s",
                 browser->persistent_state_.playing_sample_path.c_str());
    } else {
        snprintf(status_text, sizeof(status_text), "Browsing: %s", path);
        ESP_LOGI(TAG,
                 "Directory changed: Showing browsing status (is_playing=%d, path_empty=%d)",
                 browser->is_playing_ ? 1 : 0,
                 browser->persistent_state_.playing_sample_path.empty() ? 1 : 0);
    }
    browser->updateStatus(status_text);
}

// Left: where we are in the listing. Right: card state. Both were drawn by
// onEnter and never populated.
void UISampleBrowser::refreshStatusStrip() {
    if (!is_initialized_ || !listing_label_ || !lv_obj_is_valid(listing_label_)) {
        return;
    }
    const uint32_t total = file_browser_ ? wavex_file_browser_get_entry_count(file_browser_) : 0;
    const uint32_t sel = file_browser_ ? wavex_file_browser_get_selected_index(file_browser_) : 0;
    char line[96];
    if (total == 0) {
        snprintf(
            line, sizeof(line), "%s  -  empty", persistent_state_.current_directory_path.c_str());
    } else {
        snprintf(line,
                 sizeof(line),
                 "%s  -  %lu of %lu",
                 persistent_state_.current_directory_path.c_str(),
                 (unsigned long)(sel + 1),
                 (unsigned long)total);
    }
    lv_label_set_text(listing_label_, line);

    if (card_label_ && lv_obj_is_valid(card_label_)) {
        // Mount state only. Free space is not on the wire - neither
        // FileEntryWire nor StorageStatusMessage carries it - and the design's
        // "SD 12.4 GB free" would have to be invented.
        const bool mounted =
            file_browser_ ? wavex_file_browser_is_storage_mounted(file_browser_) : false;
        lv_label_set_text(card_label_, mounted ? "SD card mounted" : "No SD card");
        lv_obj_set_style_text_color(
            card_label_, lv_color_hex(mounted ? kColDim : 0xFF9800), LV_PART_MAIN);
    }
}

void UISampleBrowser::updateStatus(const char* status) {
    if (!status)
        return;

    if (!is_initialized_ || !status_label_ || !root_) {
        ESP_LOGW(TAG, "updateStatus called but UI not ready - status: %s", status);
        return;
    }

    // Store status text for deferred update (may be called from non-LVGL context)
    strncpy(pending_status_text_, status, sizeof(pending_status_text_) - 1);
    pending_status_text_[sizeof(pending_status_text_) - 1] = '\0';
    status_update_pending_.store(true, std::memory_order_release);
    wavex_ui_mark_content_changed();
    ESP_LOGD(TAG, "Status update queued: %s", status);
}

void UISampleBrowser::updateMetadata(const wavex_file_entry_t* entry) {
    if (!entry)
        return;

    if (!is_initialized_ || !metadata_label_ || !root_) {
        ESP_LOGW(TAG, "updateMetadata called but UI not ready - entry: %s", entry->name);
        return;
    }

    const bool selected_sfz = !entry->is_directory && isInstrumentFile(entry->name);
    sfz_selected_ = selected_sfz;
    if (selected_sfz) {
        if (strncmp(sfz_probe_path_, entry->path, sizeof(sfz_probe_path_)) != 0) {
            requestInstrumentProbe(entry);
        }
    } else {
        sfz_probe_ready_ = false;
        sfz_probe_loadable_ = false;
        sfz_sample_count_ = 0;
        sfz_probe_path_[0] = '\0';
        probe_request_id_.store(0, std::memory_order_release);
    }
    refreshSoftkeys();

    // Copy the entry for the deferred update (may be called from non-LVGL
    // context). A pointer would dangle: the browser's entry array is rewritten
    // per browse page in the UI domain and freed by onExit().
    pending_metadata_entry_ = *entry;
    pending_metadata_entry_valid_ = true;
    metadata_update_pending_.store(true, std::memory_order_release);
    wavex_ui_mark_content_changed();
    ESP_LOGD(TAG, "Metadata update queued for: %s", entry->name);
}

// UI task. Only a resident sample has an envelope to fetch - MSG_ENVELOPE_REQ
// is served from sample RAM - so the question is which resident sample, if
// any, the highlighted file is. Two answers, in order:
//   - the one this browser last loaded, once the backend has confirmed it.
//     Until LOAD_COMPLETE last_load_sample_id is the browser's request tag,
//     which the backend does not know: a request sent against it was dropped
//     unanswered, and the real id then had to wait out the whole timeout
//     before it could be asked for. MSG_SAMPLE_META always precedes
//     LOAD_COMPLETE, so waiting for the record costs nothing;
//   - failing that, whichever record carries this path. That is a sample
//     resident since before this boot, or one loaded by another page, which
//     "Load to preview" used to demand be loaded again to be seen.
// The lookup is cached by what it was made from, and while it has no answer
// it is retried every so often for a record that arrives later.
uint16_t UISampleBrowser::resolveSelectedSample(WaveX::Protocol::SampleMetadata& meta) {
    if (!selected_file_path_[0]) {
        return 0;
    }
    const uint16_t load_id = persistent_state_.last_load_sample_id;
    const uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
    const bool stale = strcmp(resolved_path_, selected_file_path_) != 0 ||
                       resolved_from_load_id_ != load_id ||
                       (resolved_sample_id_ == 0 && (int32_t)(now - resolve_retry_at_ms_) >= 0);
    if (stale) {
        constexpr uint32_t kResolveRetryMs = 1000;
        strncpy(resolved_path_, selected_file_path_, sizeof(resolved_path_) - 1);
        resolved_path_[sizeof(resolved_path_) - 1] = '\0';
        resolved_from_load_id_ = load_id;
        resolved_sample_id_ = 0;
        resolve_retry_at_ms_ = now + kResolveRetryMs;
        if (load_id != 0 && persistent_state_.last_load_sample_path == selected_file_path_ &&
            inter_mcu_get_sample_meta(load_id, &meta)) {
            resolved_sample_id_ = load_id;
        } else if (inter_mcu_find_sample_meta_by_name(selected_file_path_, &meta)) {
            resolved_sample_id_ = meta.sample_id;
        }
    }
    if (resolved_sample_id_ == 0) {
        return 0;
    }
    // An unload (Manage tab) drops the record; the preview goes with it.
    if (!inter_mcu_get_sample_meta(resolved_sample_id_, &meta) || meta.total_frames == 0) {
        resolved_sample_id_ = 0;
        return 0;
    }
    return resolved_sample_id_;
}

// UI task. Keeps the preview in step with what is resident and selected. The
// panel does the asking and drawing; this decides only WHICH sample, if any,
// and runs the hint label.
void UISampleBrowser::serviceWaveform() {
    if (!waveform_ || !is_initialized_ || !envelope_panel_.attached()) {
        return;
    }

    WaveX::Protocol::SampleMetadata meta;
    const uint16_t sample_id = resolveSelectedSample(meta);
    if (sample_id == 0) {
        if (shown_sample_id_ != 0) {
            envelope_panel_.clearSample();
            shown_sample_id_ = 0;
            shown_generation_ = 0;
        }
        if (waveform_hint_) {
            lv_obj_remove_flag(waveform_hint_, LV_OBJ_FLAG_HIDDEN);
        }
        return;
    }

    if (sample_id != shown_sample_id_ || meta.generation != shown_generation_) {
        shown_sample_id_ = sample_id;
        shown_generation_ = meta.generation;
        waveform_->setMonoLabel(meta.channels == 1 ? 0 : meta.channel_mode);
        envelope_panel_.setWindow(0, 0, meta.total_frames);
        envelope_panel_.setSample(sample_id, meta.generation, meta.total_frames);
    }

    // Silent on retry and give-up by design. The edit page says so on its
    // status line because the waveform is what that page is for; here it is
    // a preview beside the real information, and a browser narrating its own
    // background traffic is noise. The hint stays up until something draws.
    const uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
    if (envelope_panel_.service(now) == EnvelopePanel::Event::Drawn && waveform_hint_ &&
        envelope_panel_.drawn(0)) {
        lv_obj_add_flag(waveform_hint_, LV_OBJ_FLAG_HIDDEN);
    }
}

void UISampleBrowser::processDeferredUpdates() {
    if (s_active_instance_) {
        ESP_LOGD(TAG, "processDeferredUpdates: calling processDeferredUpdates_()");
        s_active_instance_->processDeferredUpdates_();
    } else {
        ESP_LOGD(TAG, "processDeferredUpdates: no active instance");
    }
}

void UISampleBrowser::processDeferredUpdates_() {
    if (sample_replies_.TakeOverflow() | instrument_replies_.TakeOverflow()) {
        sample_replies_.Clear();
        instrument_replies_.Clear();
        bind_on_load_track_.store(-1);
        persistent_state_.cancelLoad();
        load_request_id_.store(0);
        probe_request_id_.store(0);
        BusyOverlay::hide();
        updateStatus("Response overflow; refresh before continuing");
    }
    WaveX::Protocol::SampleStatusMessage sample;
    for (unsigned i = 0; i < 16 && sample_replies_.Pop(sample); ++i)
        applySampleStatus(sample.sample_id,
                          sample.state,
                          sample.sample_rate,
                          sample.channels,
                          sample.frames_played,
                          this);
    WaveX::Protocol::InstStatusMessage instrument;
    for (unsigned i = 0; i < 16 && instrument_replies_.Pop(instrument); ++i)
        applyInstrumentStatus(instrument, this);

    // This should be called from UI task loop with LVGL lock held

    // The waveform rides the existing UI-task pass rather than adding an
    // lv_timer of its own: this already runs at the cadence a preview needs,
    // and one fewer timer is one fewer thing to tear down on exit.
    serviceWaveform();
    const bool loading = file_browser_ && wavex_file_browser_loading(file_browser_);
    if (instruments_ && loading != tag_loading_) {
        tag_loading_ = loading;
        refreshSoftkeys();
    }

    if (status_update_pending_.load(std::memory_order_acquire) && status_label_) {
        lv_label_set_text(status_label_, pending_status_text_);
        status_update_pending_.store(false, std::memory_order_relaxed);
        ESP_LOGD(TAG, "Status label updated: %s", pending_status_text_);
    }

    // Playback position published by the sample-status callback.
    if (play_bar_update_pending_.load(std::memory_order_acquire)) {
        play_bar_update_pending_.store(false, std::memory_order_relaxed);
        if (play_bar_ && lv_obj_is_valid(play_bar_)) {
            lv_bar_set_value(
                play_bar_, pending_play_bar_pct_.load(std::memory_order_relaxed), LV_ANIM_OFF);
        }
    }

    // Softkey labels track is_playing_, which the status callback flips.
    if (softkey_refresh_pending_.exchange(false, std::memory_order_acquire)) {
        if (is_initialized_ && root_) {
            UINavigator::instance().refreshSoftkeys();
        }
    }

    // A directory listing landed: re-read the selection so metadata shows the
    // entry the cursor is actually on.
    if (selection_metadata_pending_.exchange(false, std::memory_order_acquire)) {
        if (file_browser_ && is_initialized_) {
            const uint32_t entry_count = wavex_file_browser_get_entry_count(file_browser_);
            if (entry_count > 0 && selected_file_index_ < entry_count) {
                wavex_file_browser_set_selection(file_browser_, selected_file_index_);
                const wavex_file_entry_t* selected_entry =
                    wavex_file_browser_get_selected(file_browser_);
                if (selected_entry) {
                    updateMetadata(selected_entry);
                }
            }
        }
    }

    refreshStatusStrip();
    if (file_browser_) {
        wavex_file_browser_update_loading_row(file_browser_);
    }

    if (metadata_update_pending_.load(std::memory_order_acquire) && metadata_label_) {
        char info_text[512];

        if (pending_metadata_entry_valid_) {
            const wavex_file_entry_t* entry = &pending_metadata_entry_;

            if (entry->is_directory) {
                ESP_LOGD(TAG,
                         "=== METADATA: Directory - name='%s', path='%s'",
                         entry->name,
                         entry->path);
                snprintf(info_text,
                         sizeof(info_text),
                         "Directory Information:\n"
                         "---------------------\n"
                         "Name: %.47s\n"
                         "Type: Directory\n"
                         "Path: %.95s\n\n"
                         "Use Select to enter directory",
                         entry->name,
                         entry->path);
            } else {
                ESP_LOGD(TAG,
                         "=== METADATA: File - name='%s', size=%lu, path='%s'",
                         entry->name,
                         (unsigned long)entry->size_bytes,
                         entry->path);

                if (isInstrumentFile(entry->name)) {
                    snprintf(info_text,
                             sizeof(info_text),
                             "Instrument\nInspecting referenced WAV files...\n\n"
                             "Audition is unavailable for instruments.");
                    if (detail_name_ && lv_obj_is_valid(detail_name_)) {
                        lv_label_set_text(detail_name_, entry->name);
                    }
                    lv_label_set_text(metadata_label_, info_text);
                    metadata_update_pending_.store(false, std::memory_order_relaxed);
                    pending_metadata_entry_valid_ = false;
                    return;
                }

                char size_str[32];
                if (entry->size_bytes < 1024) {
                    snprintf(size_str, sizeof(size_str), "%lu B", entry->size_bytes);
                } else if (entry->size_bytes < 1024 * 1024) {
                    snprintf(size_str,
                             sizeof(size_str),
                             "%.1f KB",
                             static_cast<float>(entry->size_bytes) / 1024.0f);
                } else {
                    snprintf(size_str,
                             sizeof(size_str),
                             "%.1f MB",
                             static_cast<float>(entry->size_bytes) / (1024.0f * 1024.0f));
                }

                // Use WAV metadata from the file entry (provided by backend)
                char duration_str[32] = "Unknown";
                char sample_rate_str[32] = "Unknown";
                char channels_str[32] = "Unknown";
                char bit_depth_str[32] = "Unknown";

                if (entry->bits_per_sample > 0) {
                    snprintf(bit_depth_str,
                             sizeof(bit_depth_str),
                             "%u-bit",
                             (unsigned)entry->bits_per_sample);
                }

                if (entry->sample_rate > 0) {
                    uint32_t duration_ms = entry->duration_ms;
                    if (duration_ms >= 60000) {  // >= 1 minute
                        int minutes = duration_ms / 60000;
                        int seconds = (duration_ms % 60000) / 1000;
                        snprintf(duration_str, sizeof(duration_str), "%dm %ds", minutes, seconds);
                    } else if (duration_ms >= 1000) {
                        snprintf(duration_str,
                                 sizeof(duration_str),
                                 "%.1fs",
                                 static_cast<float>(duration_ms) / 1000.0f);
                    } else {
                        snprintf(duration_str, sizeof(duration_str), "%lums", duration_ms);
                    }

                    if (entry->sample_rate >= 1000) {
                        snprintf(sample_rate_str,
                                 sizeof(sample_rate_str),
                                 "%.1f kHz",
                                 static_cast<float>(entry->sample_rate) / 1000.0f);
                    } else {
                        snprintf(sample_rate_str,
                                 sizeof(sample_rate_str),
                                 "%lu Hz",
                                 (unsigned long)entry->sample_rate);
                    }

                    if (entry->channels == 1) {
                        snprintf(channels_str, sizeof(channels_str), "Mono");
                    } else if (entry->channels == 2) {
                        snprintf(channels_str, sizeof(channels_str), "Stereo");
                    } else {
                        snprintf(channels_str, sizeof(channels_str), "%u ch", entry->channels);
                    }

                    ESP_LOGD(TAG,
                             "=== WAV METADATA: duration='%s', rate='%s', channels='%s', bits=%s",
                             duration_str,
                             sample_rate_str,
                             channels_str,
                             bit_depth_str);
                } else {
                    ESP_LOGD(TAG, "No WAV metadata available for: %s", entry->name);
                }

                // Two labelled rows, per design 1b. The old block repeated the
                // filename (now the headline above) and listed which softkeys
                // exist, which the softkey bar already shows.
                //
                // The design's third row - "Data offset 44 (aligned)" - is NOT
                // here: data_start is not carried by FileEntryWire or
                // SampleMetadata, and inventing a number for the one field
                // that correlated with the stutter would be worse than
                // omitting it. Plumbing it is a roadmap item.
                snprintf(info_text,
                         sizeof(info_text),
                         "Format    %s - %s - %s\n"
                         "Length    %s - %s",
                         sample_rate_str,
                         bit_depth_str,
                         channels_str,
                         duration_str,
                         size_str);
            }

            if (detail_name_ && lv_obj_is_valid(detail_name_)) {
                lv_label_set_text(detail_name_, entry->name);
            }
            lv_label_set_text(metadata_label_, info_text);

            metadata_update_pending_.store(false, std::memory_order_relaxed);
            pending_metadata_entry_valid_ = false;
            ESP_LOGD(TAG, "Metadata label updated for: %s", entry->name);
        } else if (strlen(pending_metadata_text_) > 0) {
            lv_label_set_text(metadata_label_, pending_metadata_text_);
            metadata_update_pending_.store(false, std::memory_order_relaxed);
            pending_metadata_text_[0] = '\0';
            ESP_LOGD(TAG, "Metadata label updated with pending text");
        }
    }

    if (file_browser_) {
        wavex_file_browser_process_pending_updates(file_browser_);
    }

    if (inst_status_update_pending_.exchange(false, std::memory_order_acquire)) {
        taskENTER_CRITICAL(&inst_status_lock_);
        const WaveX::Protocol::InstStatusMessage status = pending_inst_status_;
        taskEXIT_CRITICAL(&inst_status_lock_);
        if (status.op == WaveX::Protocol::INST_OP_SFZ_PROBE &&
            status.request_id == probe_request_id_.load(std::memory_order_acquire)) {
            sfz_probe_ready_ = status.state == WaveX::Protocol::INST_STATUS_PROBE_COMPLETE ||
                               status.state == WaveX::Protocol::INST_STATUS_FAILED;
            sfz_probe_loadable_ =
                status.state == WaveX::Protocol::INST_STATUS_PROBE_COMPLETE && status.flags == 0;
            sfz_sample_count_ = status.sample_count;

            char total[32], available[32], text[512];
            formatBytes(status.total_bytes, total, sizeof(total));
            formatBytes(status.available_bytes, available, sizeof(available));
            int used = snprintf(text,
                                sizeof(text),
                                "Instrument\nZones     %u\nSamples   %u\n"
                                "Total     %s\nAvailable %s",
                                (unsigned)status.zone_count,
                                (unsigned)status.sample_count,
                                total,
                                available);
            if (status.flags & WaveX::Protocol::INST_STATUS_MISSING_FILES) {
                used += snprintf(text + std::min(used, static_cast<int>(sizeof(text) - 1)),
                                 sizeof(text) - std::min(used, static_cast<int>(sizeof(text) - 1)),
                                 "\n\nWARNING: %u referenced WAV%s not found; total is incomplete.",
                                 (unsigned)status.missing_count,
                                 status.missing_count == 1 ? " was" : "s were");
            }
            if (status.flags & WaveX::Protocol::INST_STATUS_EXCEEDS_MEMORY) {
                used += snprintf(text + std::min(used, static_cast<int>(sizeof(text) - 1)),
                                 sizeof(text) - std::min(used, static_cast<int>(sizeof(text) - 1)),
                                 "\n\nWARNING: total exceeds available instrument memory.");
            }
            if (status.flags & WaveX::Protocol::INST_STATUS_INVALID_FILES) {
                snprintf(text + std::min(used, static_cast<int>(sizeof(text) - 1)),
                         sizeof(text) - std::min(used, static_cast<int>(sizeof(text) - 1)),
                         "\n\nWARNING: %u WAV%s unsupported.",
                         (unsigned)status.invalid_count,
                         status.invalid_count == 1 ? " is" : "s are");
            }
            if (status.state == WaveX::Protocol::INST_STATUS_FAILED && status.flags == 0) {
                snprintf(
                    text, sizeof(text), "Instrument inspection failed (error %u).", status.error);
            }
            if (metadata_label_ && lv_obj_is_valid(metadata_label_)) {
                lv_label_set_text(metadata_label_, text);
            }
            updateStatus(sfz_probe_loadable_ ? "Instrument ready to load"
                                             : "Instrument has load warnings");
            refreshSoftkeys();
        }
    }
}

bool UISampleBrowser::auditionSampleByIndex(uint32_t file_index) {
    ESP_LOGI(TAG,
             "=== SAMPLE PLAY INDEX OPERATION: About to audition sample by index: %lu, current "
             "is_playing=%d ===",
             (unsigned long)file_index,
             is_playing_ ? 1 : 0);

    esp_err_t result =
        comm_interface_ ? comm_interface_->sendSamplePlayRequest(file_index) : ESP_FAIL;
    if (result != ESP_OK) {
        ESP_LOGE(TAG, "Failed to send sample play index request: %d", result);
        return false;
    }

    ESP_LOGI(TAG, "=== SAMPLE PLAY INDEX OPERATION: Request sent successfully ===");
    is_playing_ = true;

    const wavex_file_entry_t* entry = wavex_file_browser_get_entry(file_browser_, file_index);
    std::string filename = entry ? entry->name : "Unknown";

    persistent_state_.startPlayback(file_index, filename);
    // Auditioning does not change which file is selected, and the selection's
    // path is what the waveform panel matches the loaded sample against.
    if (entry) {
        snprintf(selected_file_path_, sizeof(selected_file_path_), "%s", entry->path);
    }

    ESP_LOGI(TAG,
             "=== STARTED PLAYBACK: index=%d, filename='%s', persistent_path='%s'",
             file_index,
             filename.c_str(),
             persistent_state_.playing_sample_path.c_str());

    char status_text[256];
    snprintf(status_text, sizeof(status_text), "Playing: %s", filename.c_str());
    updateStatus(status_text);

    refreshSoftkeys();

    return true;
}

bool UISampleBrowser::stopAudition() {
    ESP_LOGI(TAG,
             "=== SAMPLE STOP OPERATION: Stopping sample, current is_playing=%d ===",
             is_playing_ ? 1 : 0);

    if (!is_playing_)
        return false;

    ESP_LOGI(TAG, "=== SAMPLE STOP OPERATION: Confirmed playing, proceeding with stop ===");

    esp_err_t result = comm_interface_ ? comm_interface_->sendSampleStopRequest() : ESP_FAIL;
    if (result != ESP_OK) {
        ESP_LOGE(TAG, "Failed to send sample stop request: %d", result);
        is_playing_ = false;
        updateStatus("Error: Stop failed");
        return false;
    }

    ESP_LOGI(TAG, "=== SAMPLE STOP OPERATION: Request sent successfully ===");

    // Don't immediately change is_playing - wait for response callback
    updateStatus("Stopping...");

    return true;
}

void UISampleBrowser::refreshSoftkeys() {
    if (!is_initialized_ || !root_) {
        ESP_LOGW(TAG, "refreshSoftkeys called but UI not ready");
        return;
    }

    // Queued rather than applied here: this is reached from the sample-status
    // callback on the UART task as well as from UI-task code, and rebuilding
    // the softkey row touches widgets. processDeferredUpdates_() applies it on
    // the UI task under the LVGL lock.
    //
    // This used to lv_async_call() instead, which is not a way out of the wrong
    // task: lv_async_call itself allocates and links an lv_timer, so calling it
    // off the LVGL context races the timer list it is trying to defer onto.
    softkey_refresh_pending_.store(true, std::memory_order_release);
    wavex_ui_mark_content_changed();
}

void UISampleBrowser::sample_status_callback(
    uint16_t id, uint8_t state, uint32_t rate, uint8_t channels, uint32_t frames, void* user_data) {
    auto* browser = static_cast<UISampleBrowser*>(user_data);
    if (browser)
        browser->sample_replies_.Push({id, state, channels, rate, frames});
    wavex_ui_mark_content_changed();
}

void UISampleBrowser::instrument_status_callback(const WaveX::Protocol::InstStatusMessage& status,
                                                 void* user_data) {
    auto* browser = static_cast<UISampleBrowser*>(user_data);
    if (browser)
        browser->instrument_replies_.Push(status);
    wavex_ui_mark_content_changed();
}

void UISampleBrowser::applySampleStatus(uint16_t sample_id,
                                        uint8_t state,
                                        uint32_t sample_rate,
                                        uint8_t channels,
                                        uint32_t frames_played,
                                        void* user_data) {
    // DEBUG, not INFO: LOAD_PROGRESS arrives about a hundred times per load,
    // and two lines each was enough to push everything else out of the log
    // ring before it could be read.
    ESP_LOGD(TAG,
             "=== SAMPLE STATUS CALLBACK: id=%u state=%d, rate=%lu, channels=%u, frames=%lu, "
             "user_data=%p",
             (unsigned)sample_id,
             state,
             (unsigned long)sample_rate,
             channels,
             (unsigned long)frames_played,
             user_data);

    UISampleBrowser* browser = static_cast<UISampleBrowser*>(user_data);

    if (!browser) {
        ESP_LOGE(TAG, "=== CALLBACK ERROR: browser is NULL!");
        return;
    }

    if (browser != s_active_instance_) {
        ESP_LOGW(TAG,
                 "=== CALLBACK WARNING: browser=%p != active_instance=%p, ignoring",
                 browser,
                 s_active_instance_);
        return;
    }

    if (!browser->is_initialized_) {
        ESP_LOGW(TAG, "=== CALLBACK WARNING: browser not initialized, ignoring");
        return;
    }

    if (!browser->status_label_) {
        ESP_LOGE(TAG, "=== CALLBACK ERROR: status_label_ is NULL!");
        return;
    }

    if (!browser->root_) {
        ESP_LOGE(TAG, "=== CALLBACK ERROR: root_ is NULL!");
        return;
    }

    ESP_LOGD(TAG, "=== CALLBACK VALIDATION PASSED: Processing state=%d", state);

    // State: SampleStatusState (protocol.h).
    if (state == WaveX::Protocol::SAMPLE_STATUS_STOPPED) {
        ESP_LOGI(TAG, "=== SAMPLE STOP RESPONSE: Processing stop callback ===");
        browser->is_playing_ = false;
        browser->persistent_state_.stopPlayback();

        if (browser->is_initialized_ && browser->status_label_ && browser->root_) {
            ESP_LOGD(TAG, "=== SAMPLE STOP RESPONSE: Updating UI ===");
            browser->pending_play_bar_pct_.store(0, std::memory_order_relaxed);
            browser->play_bar_update_pending_.store(true, std::memory_order_release);
            browser->updateStatus("Stopped");
            browser->refreshSoftkeys();
        } else {
            ESP_LOGW(TAG,
                     "=== SAMPLE STOP RESPONSE: Skipping UI update - not fully initialized ===");
        }
    } else if (state == WaveX::Protocol::SAMPLE_STATUS_PLAYING) {
        // Playback position. sample_rate carries the REGION LENGTH in frames
        // here, not a rate - the backend reuses the field so the UI can scale
        // without a second message. frames_played is the read position, which
        // leads the audible one by the ring (~42 ms): fine for a bar.
        if (browser->is_initialized_ && browser->root_) {
            const uint32_t region = sample_rate;
            if (region > 0) {
                const int pct = static_cast<int>(std::min<uint64_t>(
                    100, (static_cast<uint64_t>(frames_played) * 100ull) / region));
                browser->pending_play_bar_pct_.store(pct, std::memory_order_relaxed);
                browser->play_bar_update_pending_.store(true, std::memory_order_release);
            }
            if (browser->status_label_) {
                // Elapsed of total, at the sample's own rate where known.
                const uint32_t rate = browser->persistent_state_.last_load_sample_rate
                                          ? browser->persistent_state_.last_load_sample_rate
                                          : 48000u;
                const uint32_t elapsed_s = frames_played / rate;
                const uint32_t total_s = region / rate;
                char status_text[96];
                snprintf(status_text,
                         sizeof(status_text),
                         "AUDITIONING  -  %lu:%02lu / %lu:%02lu",
                         (unsigned long)(elapsed_s / 60),
                         (unsigned long)(elapsed_s % 60),
                         (unsigned long)(total_s / 60),
                         (unsigned long)(total_s % 60));
                browser->updateStatus(status_text);
            }
        }
    } else if (state == WaveX::Protocol::SAMPLE_STATUS_LOAD_PROGRESS) {
        // frames_played carries the percentage, not frames.
        BusyOverlay::requestProgress(static_cast<int>(frames_played));
        wavex_ui_mark_content_changed();
    } else if (state == WaveX::Protocol::SAMPLE_STATUS_LOAD_COMPLETE) {
        ESP_LOGI(TAG, "=== SAMPLE LOAD COMPLETE: id=%u ===", (unsigned)sample_id);
        BusyOverlay::requestHide();
        wavex_ui_mark_content_changed();
        // Refresh display-only allocator diagnostics after a successful load.
        inter_mcu_request_sample_mem_status();
        // The id in the request was only a tag: the Sample Pool assigns the
        // resident id (a file already resident answers with the id it had),
        // and this is where the frontend adopts it. One load is in flight at
        // a time - the busy overlay sees to that - so a completion while a
        // bind is pending is ours.
        const int16_t bind_track = browser->bind_on_load_track_.load(std::memory_order_acquire);
        const bool ours = bind_track >= 0 && browser->persistent_state_.completeLoad(sample_id);
        if (ours) {
            browser->bind_on_load_sample_id_.store(sample_id, std::memory_order_relaxed);
            setCurrentSampleId(sample_id);
        }
        // The second half of Load (§6.1 A): now that the id is resident, bind
        // it to the Track the user loaded onto. The Daisy answers with the
        // Track's new binding, which every page reads from the shared cache.
        const bool bound = ours;
        if (bound) {
            browser->bind_on_load_track_.store(-1, std::memory_order_release);
            inter_mcu_send_sample_select(sample_id, static_cast<uint8_t>(bind_track));
            inter_mcu_request_track_binding(static_cast<uint8_t>(bind_track));
        }
        if (browser->is_initialized_ && browser->status_label_ && browser->root_) {
            char status_text[256];
            const char* path = browser->persistent_state_.last_load_sample_path.c_str();
            const char* name = path ? strrchr(path, '/') : nullptr;
            name = name ? name + 1 : (path ? path : "");
            if (bound) {
                snprintf(status_text,
                         sizeof(status_text),
                         "%.64s loaded onto Track %u - the Keys play it",
                         name,
                         trackDisplayNumber(static_cast<uint8_t>(bind_track)));
            } else {
                snprintf(status_text,
                         sizeof(status_text),
                         "Sample loaded: id=%u (%.96s)",
                         (unsigned)sample_id,
                         name);
            }
            browser->updateStatus(status_text);
        } else {
            ESP_LOGW(TAG, "=== SAMPLE LOAD COMPLETE: Skipping UI update - not initialized ===");
        }
    } else if (state == WaveX::Protocol::SAMPLE_STATUS_LOAD_FAILED) {
        browser->persistent_state_.cancelLoad();
        // frames_played carries a SampleLoadFailReason. Before this state
        // existed a failed load left the spinner to time out; now it says why.
        browser->bind_on_load_track_.store(-1, std::memory_order_release);
        BusyOverlay::requestHide();
        wavex_ui_mark_content_changed();
        const char* why = sampleLoadFailureText(frames_played);
        ESP_LOGW(TAG,
                 "=== SAMPLE LOAD FAILED: id=%u reason=%lu ===",
                 (unsigned)sample_id,
                 (unsigned long)frames_played);
        char status_text[160];
        snprintf(status_text, sizeof(status_text), "Load failed: %s", why);
        browser->updateStatus(status_text);
    } else {
        ESP_LOGW(TAG, "=== UNKNOWN SAMPLE STATE: %d ===", state);
    }
}

void UISampleBrowser::applyInstrumentStatus(const WaveX::Protocol::InstStatusMessage& status,
                                            void* user_data) {
    UISampleBrowser* browser = static_cast<UISampleBrowser*>(user_data);
    if (!browser || browser != s_active_instance_ || !browser->is_initialized_) {
        return;
    }

    if (status.op == WaveX::Protocol::INST_OP_SFZ_PROBE) {
        if (status.request_id != browser->probe_request_id_.load(std::memory_order_acquire) ||
            status.state == WaveX::Protocol::INST_STATUS_PROBING) {
            return;
        }
        taskENTER_CRITICAL(&browser->inst_status_lock_);
        browser->pending_inst_status_ = status;
        taskEXIT_CRITICAL(&browser->inst_status_lock_);
        browser->inst_status_update_pending_.store(true, std::memory_order_release);
        wavex_ui_mark_content_changed();
        return;
    }

    if (status.op != WaveX::Protocol::INST_OP_SFZ_LOAD ||
        status.request_id != browser->load_request_id_.load(std::memory_order_acquire)) {
        return;
    }

    if (status.state == WaveX::Protocol::INST_STATUS_LOAD_BEGIN ||
        status.state == WaveX::Protocol::INST_STATUS_LOAD_PROGRESS) {
        const int total_pct =
            status.total_bytes == 0
                ? 0
                : static_cast<int>((static_cast<uint64_t>(status.loaded_bytes) * 100ull) /
                                   status.total_bytes);
        const int item_pct =
            status.current_bytes == 0
                ? 0
                : static_cast<int>((static_cast<uint64_t>(status.current_loaded_bytes) * 100ull) /
                                   status.current_bytes);
        char item[96];
        snprintf(item,
                 sizeof(item),
                 "WAV %u of %u  -  %.47s",
                 (unsigned)(status.current_index + 1u),
                 (unsigned)status.sample_count,
                 status.current_name);
        BusyOverlay::requestDualProgress(total_pct, item_pct, item);
        wavex_ui_mark_content_changed();
    } else if (status.state == WaveX::Protocol::INST_STATUS_LOAD_COMPLETE) {
        BusyOverlay::requestHide();
        browser->load_request_id_.store(0, std::memory_order_release);
        browser->updateStatus("Instrument loaded");
        wavex_ui_mark_content_changed();
    } else if (status.state == WaveX::Protocol::INST_STATUS_FAILED) {
        BusyOverlay::requestHide();
        browser->load_request_id_.store(0, std::memory_order_release);
        char message[96];
        snprintf(message, sizeof(message), "Instrument load failed (error %u)", status.error);
        browser->updateStatus(message);
        wavex_ui_mark_content_changed();
    }
}

void UISampleBrowser::requestInstrumentProbe(const wavex_file_entry_t* entry) {
    if (!entry || entry->is_directory || !isInstrumentFile(entry->name))
        return;
    snprintf(sfz_probe_path_, sizeof(sfz_probe_path_), "%s", entry->path);
    sfz_probe_ready_ = false;
    sfz_probe_loadable_ = false;
    sfz_sample_count_ = 0;
    uint32_t request_id = next_instrument_request_id_++;
    if (request_id == 0)
        request_id = next_instrument_request_id_++;
    probe_request_id_.store(request_id, std::memory_order_release);
    updateStatus("Inspecting instrument references...");
    if (inter_mcu_send_inst_op(
            request_id, getCurrentTrack(), WaveX::Protocol::INST_OP_SFZ_PROBE, entry->path) !=
        ESP_OK) {
        sfz_probe_ready_ = true;
        sfz_probe_loadable_ = false;
        updateStatus("Instrument inspection request failed");
    }
}

// Shows the Track the picker is currently on and, when that Track already
// holds something, names it and asks (§6.2 "Track n holds X - replace?") -
// overwriting an Instrument is the kind of thing that should not be
// discovered afterwards. An empty Track just asks where to load.
void UISampleBrowser::refreshTrackPrompt() {
    WaveX::Protocol::TrackBindingMessage binding;
    const bool known = inter_mcu_get_track_binding(target_track_, &binding);
    const char* what = pending_load_ == PendingLoad::Instrument ? "Instrument" : "sample";
    char prompt[160];
    const unsigned shown = trackDisplayNumber(target_track_);
    if (!known) {
        snprintf(prompt, sizeof(prompt), "Load %s onto Track %u?", what, shown);
    } else {
        switch (binding.state) {
            case WaveX::Protocol::TRACK_BINDING_PATCH:
            case WaveX::Protocol::TRACK_BINDING_LOADING:
                snprintf(prompt,
                         sizeof(prompt),
                         "Track %u holds Instrument %.23s - replace with this %s?",
                         shown,
                         binding.name[0] ? binding.name : "(unnamed)",
                         what);
                break;
            case WaveX::Protocol::TRACK_BINDING_SAMPLE: {
                WaveX::Protocol::SampleMetadata m;
                const bool named = inter_mcu_get_sample_meta(binding.sample_id, &m) && m.name[0];
                snprintf(prompt,
                         sizeof(prompt),
                         "Track %u holds sample %.32s - replace with this %s?",
                         shown,
                         named ? m.name : "(unnamed)",
                         what);
                break;
            }
            default:
                snprintf(prompt, sizeof(prompt), "Load %s onto Track %u?", what, shown);
                break;
        }
    }
    updateStatus(prompt);
}

void UISampleBrowser::askForTrack(PendingLoad kind) {
    pending_load_ = kind;
    awaiting_track_ = true;
    target_track_ = getCurrentTrack();
    refreshTrackPrompt();
    refreshSoftkeys();
}

// Workflow A (model doc §6.1): Load with a Track selected binds the sample to
// it, so the Keys play it with no further step. Rule §1.3 decides whether to
// ask first: an empty Track needs no prompt; anything else - or a Track whose
// state the Daisy has not reported yet - gets the picker, because "ask" is the
// safe reading of "unknown".
void UISampleBrowser::beginSampleLoad(const wavex_file_entry_t* entry) {
    WaveX::Protocol::TrackBindingMessage binding;
    const bool empty = inter_mcu_get_track_binding(getCurrentTrack(), &binding) &&
                       binding.state == WaveX::Protocol::TRACK_BINDING_EMPTY;
    if (!empty) {
        askForTrack(PendingLoad::Sample);
        return;
    }
    if (!loadSample(entry)) {
        ESP_LOGE(TAG, "Failed to load sample: %s", entry->name);
    }
}

bool UISampleBrowser::loadInstrument(const wavex_file_entry_t* entry) {
    if (!entry || !isInstrumentFile(entry->name) || !sfz_probe_ready_ || !sfz_probe_loadable_) {
        updateStatus("Instrument is not ready to load");
        return false;
    }
    uint32_t request_id = next_instrument_request_id_++;
    if (request_id == 0)
        request_id = next_instrument_request_id_++;
    load_request_id_.store(request_id, std::memory_order_release);

    char detail[160];
    snprintf(detail,
             sizeof(detail),
             "%s  -  %u referenced WAVs  -  Track %u",
             entry->name,
             (unsigned)sfz_sample_count_,
             trackDisplayNumber(getCurrentTrack()));
    BusyOverlay::showDual("Loading instrument", detail, 120000);
    char loading[96];
    snprintf(loading,
             sizeof(loading),
             "Loading instrument onto Track %u...",
             trackDisplayNumber(getCurrentTrack()));
    updateStatus(loading);
    const esp_err_t result = inter_mcu_send_inst_op(
        request_id, getCurrentTrack(), WaveX::Protocol::INST_OP_SFZ_LOAD, entry->path);
    if (result != ESP_OK) {
        load_request_id_.store(0, std::memory_order_release);
        BusyOverlay::hide();
        updateStatus("Instrument load request failed");
        return false;
    }
    return true;
}

bool UISampleBrowser::loadSample(const wavex_file_entry_t* entry) {
    if (!entry || strlen(entry->name) == 0) {
        ESP_LOGE(TAG, "Invalid file entry for sample loading");
        updateStatus("Error: Invalid file entry");
        return false;
    }

    if (persistent_state_.loading()) {
        updateStatus("A sample load is already pending");
        return false;
    }
    ESP_LOGI(TAG, "=== SAMPLE LOAD OPERATION: Loading sample: %s ===", entry->name);

    // Gracefully fall back if metadata isn't available from the backend yet.
    // TODO(todo1): revert to strict validation once Daisy browse metadata is populated.
    uint32_t sample_rate = entry->sample_rate ? entry->sample_rate : 44100;
    uint8_t channels = static_cast<uint8_t>(entry->channels ? entry->channels : 2);
    uint8_t bits_per_sample =
        static_cast<uint8_t>(entry->bits_per_sample ? entry->bits_per_sample : 16);

    if (entry->sample_rate == 0 || entry->channels == 0 || entry->bits_per_sample == 0) {
        ESP_LOGW(TAG,
                 "Missing WAV metadata for %s (rate=%lu, ch=%u, bits=%u). "
                 "Using defaults %lu Hz, %u ch, %u-bit.",
                 entry->name,
                 (unsigned long)entry->sample_rate,
                 entry->channels,
                 entry->bits_per_sample,
                 (unsigned long)sample_rate,
                 channels,
                 bits_per_sample);
    }

    if (!WaveX::Wav::IsResidentSampleFormatSupported(bits_per_sample, channels)) {
        ESP_LOGE(TAG,
                 "Unsupported resident sample format: %u-bit, %u channels (requires PCM16 "
                 "mono/stereo)",
                 bits_per_sample,
                 channels);
        updateStatus("Error: Load supports PCM16 mono/stereo; use Audition for PCM24");
        return false;
    }

    // The backend owns admission: resident reuse needs no new PCM allocation,
    // WAV container bytes differ from PCM bytes, and cached free RAM can be stale.
    // A request tag, not the resident id: the Daisy's Sample Pool assigns
    // that and reports it with LOAD_COMPLETE, where the browser adopts it.
    uint16_t sample_id = persistent_state_.allocateSampleId();
    // Bind it to the selected Track when the Daisy says it is resident: the
    // bind is a separate message (MSG_SAMPLE_SELECT) and would be refused
    // for an id that is not loaded yet.
    bind_on_load_sample_id_.store(sample_id, std::memory_order_relaxed);
    bind_on_load_track_.store(static_cast<int16_t>(getCurrentTrack()), std::memory_order_release);
    auto pending_entry = *entry;
    pending_entry.sample_rate = sample_rate;
    pending_entry.channels = channels;
    pending_entry.bits_per_sample = bits_per_sample;
    persistent_state_.stageLoad(sample_id, pending_entry);

    {
        // The ESP32 is not blocked here - the Daisy does the SD read and
        // answers over the link - so LVGL keeps redrawing and this spinner
        // genuinely spins. Timeout is generous: a large sample off a slow card
        // legitimately takes seconds.
        char detail[160];
        snprintf(detail,
                 sizeof(detail),
                 "%s  -  %.1f MB",
                 entry->name,
                 static_cast<float>(entry->size_bytes) / (1024.0f * 1024.0f));
        BusyOverlay::show("Loading sample", detail, 20000);
    }
    updateStatus("Loading sample on Daisy...");
    // The wire's sample_rate is a uint16_t hint (Daisy re-reads the real rate
    // from the file); a rate that cannot fit degrades to 0 = unknown rather
    // than wrapping to a wrong-but-plausible value.
    const uint16_t rate_hint = sample_rate <= UINT16_MAX ? static_cast<uint16_t>(sample_rate) : 0;
    esp_err_t result =
        comm_interface_
            ? inter_mcu_send_sample_load_req(
                  sample_id, entry->size_bytes, rate_hint, channels, bits_per_sample, entry->path)
            : ESP_FAIL;

    if (result != ESP_OK) {
        ESP_LOGE(TAG, "Failed to send sample load request: %d", result);
        persistent_state_.cancelLoad();
        bind_on_load_track_.store(-1, std::memory_order_release);
        BusyOverlay::hide();
        updateStatus("Error: Load request failed");
        return false;
    }

    ESP_LOGI(TAG,
             "=== SAMPLE LOAD REQUEST SENT TO DAISY: id=%u path=%s ===",
             (unsigned)sample_id,
             entry->path);
    updateStatus("Sample load requested on Daisy");

    return true;
}

// Debug harness (docs/features/debug-harness-and-hil.md §4): what a test
// needs to assert the Load-to-Track workflow, and the two set-up commands
// that make a test independent of what is on the card and where the
// highlight was left. Neither bypasses the thing under test - Load itself
// still goes through the softkey.
size_t UISampleBrowser::consoleState(char* out, size_t cap, size_t len) {
    using namespace WaveX::Debug;
    len = AppendKvInt(out, cap, len, "tagfilter", persistent_state_.tag_filter);
    len = AppendKvText(
        out, cap, len, "status", status_label_ ? lv_label_get_text(status_label_) : "");
    const wavex_file_entry_t* sel =
        file_browser_ ? wavex_file_browser_get_selected(file_browser_) : nullptr;
    len = AppendKvText(out, cap, len, "sel", sel ? sel->name : "-");
    len = AppendKvText(out,
                       cap,
                       len,
                       "dir",
                       file_browser_ ? wavex_file_browser_get_current_path(file_browser_) : "-");
    len = AppendKvInt(
        out,
        cap,
        len,
        "entries",
        file_browser_ ? static_cast<long>(wavex_file_browser_get_entry_count(file_browser_)) : 0);
    len = AppendKvInt(out, cap, len, "picker", awaiting_track_ ? 1 : 0);
    len = AppendKvInt(out, cap, len, "target", target_track_);
    len = AppendKvInt(out, cap, len, "playing", is_playing_ ? 1 : 0);
    len = AppendKvInt(out, cap, len, "lastid", persistent_state_.last_load_sample_id);
    return len;
}

bool UISampleBrowser::consoleCommand(const char* args, char* reply, size_t cap) {
    unsigned tag;
    char extra;
    if (instruments_ && std::sscanf(args, "TAGFILTER %u %c", &tag, &extra) == 1 && tag <= 8 &&
        file_browser_ && !wavex_file_browser_loading(file_browser_) && !awaiting_track_) {
        changeTagFilter(static_cast<uint8_t>(tag));
        std::snprintf(reply, cap, "OK");
        return true;
    }

    using namespace WaveX::Debug;
    char verb[16];
    const char* p = args;
    if (!NextWord(&p, verb, sizeof(verb)) || !file_browser_) {
        snprintf(reply, cap, "nobrowser");
        return false;
    }
    p = detail::SkipSpaces(p);
    if (!strcmp(verb, "DIR")) {
        // DIR <path>: list a directory, as navigating into it would.
        if (!*p || !wavex_file_browser_navigate_to(file_browser_, p)) {
            snprintf(reply, cap, "nodir");
            return false;
        }
        refreshSoftkeys();
        reply[0] = '\0';
        return true;
    }
    if (!strcmp(verb, "SEL")) {
        // SEL <name>: move the highlight to a listed entry by name, firing
        // the same selection callback the encoder does.
        const uint32_t n = wavex_file_browser_get_entry_count(file_browser_);
        for (uint32_t i = 0; i < n; ++i) {
            const wavex_file_entry_t* e = wavex_file_browser_get_entry(file_browser_, i);
            if (e && strcmp(e->name, p) == 0) {
                wavex_file_browser_set_selection(file_browser_, i);
                file_selected_index_callback(i, e, this);
                refreshSoftkeys();
                snprintf(reply, cap, "index=%lu", static_cast<unsigned long>(i));
                return true;
            }
        }
        snprintf(reply, cap, "noentry");
        return false;
    }
    snprintf(reply, cap, "unknown");
    return false;
}

namespace {
SampleBrowserState& persistent_state_singleton() {
    static SampleBrowserState persistent_state;
    return persistent_state;
}
}  // namespace

std::shared_ptr<UIPage> createSampleBrowserPage(WaveX::Comm::ICommInterface& comm_interface) {
    return std::make_shared<UISampleBrowser>(comm_interface, persistent_state_singleton());
}

std::shared_ptr<UIPage> createInstrumentBrowserPage() {
    static SampleBrowserState state;
    auto comm = ui_get_comm_interface();
    return comm ? std::make_shared<UISampleBrowser>(*comm, state, true) : nullptr;
}

SampleBrowserState* getSampleBrowserState() {
    return &persistent_state_singleton();
}

}  // namespace wavex_ui
