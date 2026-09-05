// WaveX UI Sample Browser Implementation
#include "ui/ui_sample_browser.h"

#include <esp_log.h>
#include <esp_timer.h>
#include <strings.h>

#include "../components/envelope_cache.h"
#include "../components/waveform_view.h"
#include "../styles/ui_theme.h"
#include "comm/i_comm_interface.h"
#include "debug/console_command.h"
#include "esp_lvgl_port.h"
#include "inter_mcu.h"
#include "ui/current_sample.h"
#include "ui/current_track.h"
#include "ui/ui_busy_overlay.h"
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

constexpr uint32_t kColPanel = 0x0E0E0E;
constexpr uint32_t kColBorder = 0x222222;
constexpr uint32_t kColDim = 0x8FA0AA;
constexpr uint32_t kColGreen = 0x4CAF50;

// Waveform preview, in the gap the design leaves between the filename headline
// and the metadata rows.
constexpr int kWaveX = 16;
constexpr int kWaveY = 58;
constexpr int kWaveW = kDetailW - 32;
constexpr int kWaveH = 150;

// Columns asked of the envelope. The panel is 442 px wide, and asking for more
// columns than pixels buys nothing; a coarse whole-file view is also the tier
// most likely to be cached already from a previous visit.
constexpr uint16_t kWaveColumns = 442;
constexpr uint32_t kWaveTimeoutMs = 3000;
constexpr uint8_t kWaveMaxRetries = 2;

bool SendEnvelopeReq(uint16_t sample_id, uint16_t columns, uint32_t start, uint32_t end) {
    return inter_mcu_send_envelope_req(sample_id, columns, start, end) == ESP_OK;
}

bool isSfzFile(const char* name) {
    if (!name)
        return false;
    const char* dot = strrchr(name, '.');
    return dot && strcasecmp(dot, ".sfz") == 0;
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
                                 SampleBrowserState& persistent_state)
    : comm_interface_(&comm_interface), persistent_state_(persistent_state) {}

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
    lv_obj_set_style_text_font(listing_label_, &lv_font_montserrat_18, LV_PART_MAIN);
    lv_obj_set_style_text_color(listing_label_, lv_color_hex(kColDim), LV_PART_MAIN);
    lv_label_set_text(listing_label_, "");
    lv_obj_set_pos(listing_label_, kMargin, kStatusY);

    card_label_ = lv_label_create(root_);
    lv_obj_set_style_text_font(card_label_, &lv_font_montserrat_18, LV_PART_MAIN);
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
    lv_obj_set_style_text_font(detail_name_, &lv_font_montserrat_26, LV_PART_MAIN);
    lv_obj_set_style_text_color(detail_name_, UI_COLOR_TEXT, LV_PART_MAIN);
    lv_obj_set_pos(detail_name_, 16, 14);
    lv_obj_set_width(detail_name_, kDetailW - 32);
    lv_label_set_long_mode(detail_name_, LV_LABEL_LONG_DOT);
    lv_label_set_text(detail_name_, "Select a file");

    // Waveform preview of the loaded sample. Built unconditionally so the
    // panel's geometry does not shift when one appears; it simply draws its
    // zero line until there is something to show.
    EnsureEnvelopeCacheInitialised();
    envelope_columns_.assign(static_cast<size_t>(kWaveColumns) * 2,
                             WaveX::Protocol::EnvelopeColumn());

    EnvelopeFetcher::Config wave_cfg;
    wave_cfg.display_columns = kWaveColumns;
    wave_cfg.max_run_columns = WaveX::Protocol::MAX_ENVELOPE_COLUMNS;
    wave_cfg.timeout_ms = kWaveTimeoutMs;
    // Fewer retries than the edit page: this is a preview beside the real
    // information, not the thing the user came to look at, and a browser that
    // keeps retrying in the background is traffic nobody asked for.
    wave_cfg.max_retries = kWaveMaxRetries;
    envelope_fetcher_.init(wave_cfg, &SendEnvelopeReq, &GetEnvelopeCache());

    lv_obj_t* wave_panel = lv_obj_create(info_panel_);
    lv_obj_remove_style_all(wave_panel);
    lv_obj_set_size(wave_panel, kWaveW, kWaveH);
    lv_obj_set_pos(wave_panel, kWaveX, kWaveY);
    lv_obj_set_style_bg_color(wave_panel, lv_color_hex(0x0A0A0A), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(wave_panel, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(wave_panel, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(wave_panel, lv_color_hex(kColBorder), LV_PART_MAIN);
    lv_obj_remove_flag(wave_panel, LV_OBJ_FLAG_SCROLLABLE);
    waveform_ = std::make_unique<WaveformView>(wave_panel, lv_pct(100), lv_pct(100));

    // Says why the panel is empty. Without it an unloaded selection looks
    // identical to a preview that failed, which is the ambiguity the busy
    // overlay was criticised for elsewhere.
    waveform_hint_ = lv_label_create(info_panel_);
    lv_obj_set_style_text_font(waveform_hint_, &lv_font_montserrat_18, LV_PART_MAIN);
    lv_obj_set_style_text_color(waveform_hint_, lv_color_hex(kColDim), LV_PART_MAIN);
    lv_obj_set_pos(waveform_hint_, kWaveX + 10, kWaveY + (kWaveH / 2) - 12);
    lv_label_set_text(waveform_hint_, "Load to preview");

    inter_mcu_set_envelope_chunk_listener(&UISampleBrowser::envelopeChunkStatic, this);

    // Metadata rows. Kept as one wrapped label rather than a table: the
    // fields are fixed and a table's chrome costs more than it adds here.
    metadata_label_ = lv_label_create(info_panel_);
    lv_obj_set_style_text_font(metadata_label_, &lv_font_montserrat_18, LV_PART_MAIN);
    lv_obj_set_style_text_color(metadata_label_, lv_color_hex(kColDim), LV_PART_MAIN);
    lv_obj_set_pos(metadata_label_, 16, 222);
    lv_obj_set_width(metadata_label_, kDetailW - 32);
    lv_label_set_long_mode(metadata_label_, LV_LABEL_LONG_WRAP);
    lv_label_set_text(metadata_label_, "");

    status_label_ = lv_label_create(info_panel_);
    lv_obj_set_style_text_font(status_label_, &lv_font_montserrat_18, LV_PART_MAIN);
    lv_obj_set_style_text_color(status_label_, lv_color_hex(kColGreen), LV_PART_MAIN);
    lv_obj_set_pos(status_label_, 16, 458);
    lv_obj_set_width(status_label_, kDetailW - 32);
    lv_label_set_long_mode(status_label_, LV_LABEL_LONG_DOT);
    lv_label_set_text(status_label_, "Ready");

    play_bar_ = lv_bar_create(info_panel_);
    lv_obj_set_size(play_bar_, kDetailW - 32, 10);
    lv_obj_set_pos(play_bar_, 16, 490);
    lv_bar_set_range(play_bar_, 0, 100);
    lv_bar_set_value(play_bar_, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(play_bar_, lv_color_hex(0x1F1F1F), LV_PART_MAIN);
    lv_obj_set_style_bg_color(play_bar_, lv_color_hex(kColGreen), LV_PART_INDICATOR);

    wavex_file_browser_config_t browser_config = {.root_path = persistent_state_.current_directory_path.c_str(), .file_extension = nullptr, .max_entries = 50, .show_hidden = false, .comm_interface = comm_interface_};

    ESP_LOGI(TAG, "Creating file browser with root_path: %s", browser_config.root_path);

    file_browser_ = wavex_file_browser_create(browser_container_, &browser_config);
    if (!file_browser_) {
        ESP_LOGE(TAG, "Failed to create file browser");
        return;
    }

    // Warm the allocator view so the first load's fit check has real numbers
    // rather than a zeroed cache (which the check reads as "unknown, allow").
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
    // A half-answered picker does not survive leaving the page, and a load
    // whose completion we will no longer hear must not bind later either.
    awaiting_track_ = false;
    pending_load_ = PendingLoad::None;
    bind_on_load_track_.store(-1, std::memory_order_release);
    // Unregister before the widgets go, or the RX task would be writing
    // through a freed page. abort() then releases the cache arming: skipping
    // it would leave noteRequest() blocking every later waveform in the
    // process, not just this page's.
    inter_mcu_set_envelope_chunk_listener(nullptr, nullptr);
    envelope_fetcher_.abort();
    waveform_.reset();
    waveform_hint_ = nullptr;
    shown_sample_id_ = 0;
    shown_generation_ = 0;
    waveform_drawn_ = false;
    waveform_gave_up_ = false;
    if (s_active_instance_ == this) {
        s_active_instance_ = nullptr;
    }

    // Mark as not initialized to prevent any stray callbacks
    is_initialized_ = false;

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

            // Handle each step of delta separately for responsive scrolling
            int steps = (evt.delta > 0) ? evt.delta : -evt.delta;
            bool result = false;

            for (int step = 0; step < steps && step < 10; step++) {
                if (evt.type == InputType::EncoderUp) {
                    result = wavex_file_browser_navigate_up_entry(file_browser_);
                } else {
                    result = wavex_file_browser_navigate_down_entry(file_browser_);
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

std::array<Softkey, NUM_SOFTKEYS> UISampleBrowser::getSoftkeys() {
    std::array<Softkey, NUM_SOFTKEYS> keys{};
    const wavex_file_entry_t* selected =
        file_browser_ ? wavex_file_browser_get_selected(file_browser_) : nullptr;
    const bool selected_sfz = selected && !selected->is_directory && isSfzFile(selected->name);

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
        // A sample cannot take over a Track that holds an imported Instrument:
        // the import owns its samples and only the load handshake releases
        // them (SfzLoader::BindSample refuses). Until the Sample Pool (model
        // doc §4) that Track is simply not a valid target, and the picker
        // says so rather than sending a bind the Daisy will ignore.
        WaveX::Protocol::TrackBindingMessage binding;
        if (pending_load_ == PendingLoad::Sample &&
            inter_mcu_get_track_binding(target_track_, &binding) &&
            (binding.state == WaveX::Protocol::TRACK_BINDING_PATCH ||
             binding.state == WaveX::Protocol::TRACK_BINDING_LOADING)) {
            keys[3].enabled = false;
            keys[3].why =
                "Holds an Instrument - a sample cannot replace it yet; pick another Track";
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
            keys[1] = {"Audition", nullptr, false, "SFZ instruments cannot be auditioned"};
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
            if (isSfzFile(selected->name)) {
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
    strncpy(browser->selected_file_path_, entry->name, sizeof(browser->selected_file_path_) - 1);
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
    browser->pending_metadata_entry_ = nullptr;
    browser->metadata_update_pending_.store(true, std::memory_order_release);
    wavex_ui_mark_content_changed();

    // Show metadata for the selected file once the listing has landed. This
    // runs on the UART task, so it only raises a flag; processDeferredUpdates_()
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

    const bool selected_sfz = !entry->is_directory && isSfzFile(entry->name);
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

    // Store entry pointer for deferred update (may be called from non-LVGL context)
    pending_metadata_entry_ = entry;
    metadata_update_pending_.store(true, std::memory_order_release);
    wavex_ui_mark_content_changed();
    ESP_LOGD(TAG, "Metadata update queued for: %s", entry->name);
}

// Static method to process updates for active instance (called from UI task)
// UART RX task. Ordering is the fetcher's problem; nothing here may touch
// LVGL or the cache.
void UISampleBrowser::envelopeChunkStatic(const WaveX::Protocol::EnvelopeChunkMessage& header,
                                          const WaveX::Protocol::EnvelopeColumn* columns,
                                          void* user) {
    auto* self = static_cast<UISampleBrowser*>(user);
    if (self) {
        self->envelope_fetcher_.onChunk(header, columns);
    }
}

bool UISampleBrowser::selectionIsLoadedSample() const {
    if (persistent_state_.last_load_sample_id == 0 ||
        persistent_state_.last_load_sample_path.empty()) {
        return false;
    }
    return persistent_state_.last_load_sample_path == selected_file_path_;
}

// UI task. Keeps the preview in step with what is loaded and selected.
void UISampleBrowser::serviceWaveform() {
    if (!waveform_ || !is_initialized_) {
        return;
    }

    // Only the loaded sample has an envelope to fetch - MSG_ENVELOPE_REQ is
    // served from sample RAM, so a merely-selected file has nothing behind it.
    if (!selectionIsLoadedSample()) {
        if (shown_sample_id_ != 0) {
            waveform_->clear();
            shown_sample_id_ = 0;
            shown_generation_ = 0;
            waveform_drawn_ = false;
            waveform_gave_up_ = false;
        }
        if (waveform_hint_) {
            lv_obj_remove_flag(waveform_hint_, LV_OBJ_FLAG_HIDDEN);
        }
        return;
    }

    const uint16_t sample_id = persistent_state_.last_load_sample_id;
    uint16_t generation = 0;
    uint32_t total_frames = 0;
    WaveX::Protocol::SampleMetadata meta;
    if (inter_mcu_get_sample_meta(sample_id, &meta) && meta.total_frames > 0) {
        generation = meta.generation;
        total_frames = meta.total_frames;
    } else {
        // The record has not arrived yet. Fall back to the geometry the browse
        // listing carried, so the first request is not delayed a whole round
        // trip; if it turns out to overshoot, the backend clamps and the
        // generation check below re-draws once the real record lands.
        total_frames = persistent_state_.lastLoadFrames();
    }
    if (total_frames == 0) {
        return;  // nothing to ask about yet
    }

    if (sample_id != shown_sample_id_ || generation != shown_generation_) {
        shown_sample_id_ = sample_id;
        shown_generation_ = generation;
        waveform_drawn_ = false;
        waveform_gave_up_ = false;
    }

    // The steady state, and the reason this function is cheap to call at the
    // UI task's rate: once drawn with nothing in flight there is nothing to do
    // until the selection or the sample's content changes.
    if ((waveform_drawn_ || waveform_gave_up_) && !envelope_fetcher_.busy()) {
        return;
    }

    const uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
    bool redraw = false;

    switch (envelope_fetcher_.service(now)) {
        case EnvelopeFetcher::Service::Committed:
            redraw = true;
            break;
        case EnvelopeFetcher::Service::Retrying:
            // Draw the partial run rather than holding the panel blank.
            redraw = true;
            break;
        case EnvelopeFetcher::Service::GaveUp:
            // Silent by design. The edit page says so on its status line
            // because the waveform is what that page is for; here it is a
            // preview beside the real information, and a browser narrating its
            // own background traffic is noise.
            waveform_gave_up_ = true;
            redraw = true;
            break;
        case EnvelopeFetcher::Service::Idle:
            break;
    }

    if (!waveform_gave_up_) {
        // Whole file, always: this panel does not zoom, so one coarse tier
        // serves it and stays cached cheaply across selections.
        if (envelope_fetcher_.request(sample_id, generation, 0, total_frames, total_frames, now) ==
            EnvelopeFetcher::Request::AlreadyCached) {
            redraw = true;
        }
    }

    if (redraw) {
        drawWaveform();
    }
}

void UISampleBrowser::drawWaveform() {
    if (!waveform_ || envelope_columns_.empty() || shown_sample_id_ == 0) {
        return;
    }
    uint32_t total_frames = 0;
    WaveX::Protocol::SampleMetadata meta;
    if (inter_mcu_get_sample_meta(shown_sample_id_, &meta) && meta.total_frames > 0) {
        total_frames = meta.total_frames;
    } else {
        total_frames = persistent_state_.lastLoadFrames();
    }
    if (total_frames == 0) {
        return;
    }

    uint8_t channels = 1;
    const uint16_t drawn = GetEnvelopeCache().render(shown_sample_id_,
                                                     shown_generation_,
                                                     0,
                                                     total_frames,
                                                     kWaveColumns,
                                                     envelope_columns_.data(),
                                                     envelope_columns_.size(),
                                                     channels);
    if (drawn == 0) {
        return;  // nothing cached yet; the request is in flight
    }
    waveform_->setEnvelope(envelope_columns_.data(), kWaveColumns, channels);
    waveform_drawn_ = true;
    if (waveform_hint_) {
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
    // This should be called from UI task loop with LVGL lock held

    // The waveform rides the existing UI-task pass rather than adding an
    // lv_timer of its own: this already runs at the cadence a preview needs,
    // and one fewer timer is one fewer thing to tear down on exit.
    serviceWaveform();

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

        if (pending_metadata_entry_) {
            const wavex_file_entry_t* entry = pending_metadata_entry_;

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

                if (isSfzFile(entry->name)) {
                    snprintf(info_text,
                             sizeof(info_text),
                             "SFZ instrument\nInspecting referenced WAV files...\n\n"
                             "Audition is unavailable for instruments.");
                    if (detail_name_ && lv_obj_is_valid(detail_name_)) {
                        lv_label_set_text(detail_name_, entry->name);
                    }
                    lv_label_set_text(metadata_label_, info_text);
                    metadata_update_pending_.store(false, std::memory_order_relaxed);
                    pending_metadata_entry_ = nullptr;
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
            pending_metadata_entry_ = nullptr;
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
                                "SFZ instrument\nZones     %u\nSamples   %u\n"
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
                snprintf(text, sizeof(text), "SFZ inspection failed (error %u).", status.error);
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
    snprintf(selected_file_path_, sizeof(selected_file_path_), "%s", filename.c_str());

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

void UISampleBrowser::sample_status_callback(uint16_t sample_id,
                                             uint8_t state,
                                             uint32_t sample_rate,
                                             uint8_t channels,
                                             uint32_t frames_played,
                                             void* user_data) {
    ESP_LOGI(TAG,
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

    ESP_LOGI(TAG, "=== CALLBACK VALIDATION PASSED: Processing state=%d", state);

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
        // Refresh the allocator view so the next load's fit check is against
        // what is actually free now, not what was free before this one.
        inter_mcu_request_sample_mem_status();
        // The id in the request was only a tag: the Sample Pool assigns the
        // resident id (a file already resident answers with the id it had),
        // and this is where the frontend adopts it. One load is in flight at
        // a time - the busy overlay sees to that - so a completion while a
        // bind is pending is ours.
        const int16_t bind_track = browser->bind_on_load_track_.load(std::memory_order_acquire);
        const bool ours = bind_track >= 0;
        if (ours) {
            browser->persistent_state_.last_load_sample_id = sample_id;
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
        // frames_played carries a SampleLoadFailReason. Before this state
        // existed a failed load left the spinner to time out; now it says why.
        browser->bind_on_load_track_.store(-1, std::memory_order_release);
        BusyOverlay::requestHide();
        wavex_ui_mark_content_changed();
        const char* why;
        switch (frames_played) {
            case WaveX::Protocol::SAMPLE_LOAD_FAIL_NO_SDRAM:
                why = "sample memory is unavailable on the Daisy";
                break;
            case WaveX::Protocol::SAMPLE_LOAD_FAIL_OPEN:
                why = "the Daisy could not open the file";
                break;
            case WaveX::Protocol::SAMPLE_LOAD_FAIL_FORMAT:
                why = "not a resident-playable WAV (PCM16 mono/stereo)";
                break;
            case WaveX::Protocol::SAMPLE_LOAD_FAIL_RAM:
                why = "does not fit in free sample RAM";
                break;
            case WaveX::Protocol::SAMPLE_LOAD_FAIL_READ:
                why = "SD read error during the load";
                break;
            case WaveX::Protocol::SAMPLE_LOAD_FAIL_REGISTRY_FULL:
                why = "too many samples resident - unload one";
                break;
            default:
                why = "unknown reason";
                break;
        }
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

void UISampleBrowser::instrument_status_callback(const WaveX::Protocol::InstStatusMessage& status,
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
    if (!entry || entry->is_directory || !isSfzFile(entry->name))
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
    if (!entry || !isSfzFile(entry->name) || !sfz_probe_ready_ || !sfz_probe_loadable_) {
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

    // Will it fit? The Daisy reports its allocator state in
    // SampleMemStatusMessage, cached here from the last status. Checking the
    // LARGEST FREE BLOCK, not total free: the allocator hands out contiguous
    // extents, so a fragmented pool with plenty of total space still cannot
    // take a big sample.
    wavex_sample_mem_status_t mem{};
    inter_mcu_get_sample_mem_status(&mem);
    if (mem.largest_free_bytes > 0 && entry->size_bytes > mem.largest_free_bytes) {
        char warn[192];
        snprintf(warn, sizeof(warn), "%.1f MB sample, largest free block is %.1f MB", static_cast<float>(entry->size_bytes) / (1024.0f * 1024.0f), static_cast<float>(mem.largest_free_bytes) / (1024.0f * 1024.0f));
        ESP_LOGW(TAG, "Sample will not fit: %s", warn);
        BusyOverlay::show("Sample will not fit", warn, 6000);
        // Partial load would need a length field on MSG_SAMPLE_LOAD and a
        // truncating reader on the Daisy - roadmap Phase 1.5.5. Refusing with
        // the numbers on screen beats a failed load with no explanation.
        updateStatus("Error: sample too large for free sample RAM");
        return false;
    }

    // A request tag, not the resident id: the Daisy's Sample Pool assigns
    // that and reports it with LOAD_COMPLETE, where the browser adopts it.
    uint16_t sample_id = persistent_state_.allocateSampleId();
    persistent_state_.last_load_sample_id = sample_id;
    // Bind it to the selected Track when the Daisy says it is resident: the
    // bind is a separate message (MSG_SAMPLE_SELECT) and would be refused
    // for an id that is not loaded yet.
    bind_on_load_sample_id_.store(sample_id, std::memory_order_relaxed);
    bind_on_load_track_.store(static_cast<int16_t>(getCurrentTrack()), std::memory_order_release);
    persistent_state_.last_load_sample_path = entry->path;
    setCurrentSampleId(sample_id);
    // Capture the geometry too - the edit page has no other source for it.
    persistent_state_.last_load_sample_rate = sample_rate;
    persistent_state_.last_load_duration_ms = entry->duration_ms;
    persistent_state_.last_load_channels = channels;
    persistent_state_.last_load_bits = bits_per_sample;
    persistent_state_.last_load_size_bytes = entry->size_bytes;

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

SampleBrowserState* getSampleBrowserState() {
    return &persistent_state_singleton();
}

}  // namespace wavex_ui
