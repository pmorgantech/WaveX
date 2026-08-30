// WaveX UI Diagnostics Page Implementation
#include "ui/ui_diagnostics_page.h"

#include <esp_log.h>
#include <esp_lvgl_port.h>
#include <string.h>

#include "comm/statistics.h"
#include "config.h"
#include "config/link_config.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "inter_mcu.h"
#include "links/esp_spi_link.h"
#include "ui/ui_navigator.h"
#include "ui/ui_sample_memory_page.h"
#include "ui_task.h"

#include <memory>

static const char* TAG = "UI_DIAGNOSTICS_PAGE";

namespace wavex_ui {

UIDiagnosticsPage::UIDiagnosticsPage()
    : cpu_usage_percent(0.0f),
      cpu_usage_core0(0.0f),
      cpu_usage_core1(0.0f),
      cpu_measurement_count(0),
      last_total_runtime(0),
      last_idle_runtime_core0(0),
      last_idle_runtime_core1(0),
      last_check_time_ms(0),
      last_esp_idf_check_time(0),
      diagnostics_timer_handle(nullptr),
      lvgl_update_timer(nullptr),
      tabview(nullptr),
      active_tab(TAB_SYSTEM),
      frozen(false),
      midi_note(nullptr),
      msg_table(nullptr),
      ui_update_pending(false) {
    memset(sys_cards, 0, sizeof(sys_cards));
    memset(link_cards, 0, sizeof(link_cards));
    memset(audio_cards, 0, sizeof(audio_cards));
    memset(storage_cards, 0, sizeof(storage_cards));
    memset(midi_cards, 0, sizeof(midi_cards));
    memset(cpu_usage_history, 0, sizeof(cpu_usage_history));
}

UIDiagnosticsPage::~UIDiagnosticsPage() {
    // Cleanup is done in onExit()
}

const char* UIDiagnosticsPage::name() const {
    return "Diagnostics";
}

namespace {

// Design palette (WaveX Wireframes v2). ui_theme.h covers the shared subset;
// these are the additional greys the diagnostics cards use.
constexpr uint32_t kColBg = 0x000000;
constexpr uint32_t kColCard = 0x141414;
constexpr uint32_t kColBorder = 0x333333;
constexpr uint32_t kColDim = 0x8FA0AA;
constexpr uint32_t kColDimmer = 0x6E7A82;
constexpr uint32_t kColTrack = 0x262B2E;
constexpr uint32_t kColTabOn = 0x10293B;
constexpr uint32_t kColGreen = 0x4CAF50;
constexpr uint32_t kColOrange = 0xFF5722;
constexpr uint32_t kColBlue = 0x2196F3;

// Card grid, from the design: 4 across, 305x226, gutters 12.
constexpr int kCardW = 305;
constexpr int kCardH = 226;
constexpr int kGaugeW = 273;
constexpr int kColX[4] = {12, 329, 646, 963};
constexpr int kRowY[2] = {0, 238};  // relative to the tab body

lv_obj_t* mkLabel(
    lv_obj_t* parent, int x, int y, const char* txt, const lv_font_t* font, uint32_t colour) {
    lv_obj_t* l = lv_label_create(parent);
    lv_label_set_text(l, txt);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(colour), 0);
    lv_obj_set_pos(l, x, y);
    return l;
}

}  // namespace

void UIDiagnosticsPage::onEnter(lv_obj_t* parent) {
    ESP_LOGI(TAG, "Diagnostics page entering");

    lv_obj_clean(parent);
    msg_table = nullptr;
    frozen = false;
    memset(sys_cards, 0, sizeof(sys_cards));
    memset(link_cards, 0, sizeof(link_cards));

    buildTabs(parent);
    startDiagnosticsMonitoring();
    // Subscribe only while this page is open. 2 Hz is ~188 B/s against a
    // 200 KB/s link; closed, the backend sends nothing at all.
    inter_mcu_send_diag_subscribe(true, 2);
}

void UIDiagnosticsPage::buildTabs(lv_obj_t* parent) {
    tabview = lv_tabview_create(parent);
    lv_tabview_set_tab_bar_size(tabview, 56);
    lv_obj_set_size(tabview, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(tabview, lv_color_hex(kColBg), 0);
    lv_obj_set_style_border_width(tabview, 0, 0);
    lv_obj_set_style_pad_all(tabview, 0, 0);

    lv_obj_t* bar = lv_tabview_get_tab_bar(tabview);
    lv_obj_set_style_bg_color(bar, lv_color_hex(kColBg), 0);
    lv_obj_set_style_text_font(bar, &lv_font_montserrat_22, 0);
    lv_obj_set_style_text_color(bar, lv_color_hex(kColDimmer), 0);
    // Selected tab cell: filled, white text, 4px blue underline. Composed once
    // because mixing lv_part_t with lv_state_t warns under C++20.
    const lv_style_selector_t sel_on = static_cast<lv_style_selector_t>(LV_PART_ITEMS) |
                                       static_cast<lv_style_selector_t>(LV_STATE_CHECKED);
    lv_obj_set_style_bg_color(bar, lv_color_hex(kColTabOn), sel_on);
    lv_obj_set_style_text_color(bar, lv_color_white(), sel_on);
    lv_obj_set_style_border_color(bar, lv_color_hex(kColBlue), sel_on);
    lv_obj_set_style_border_width(bar, 4, sel_on);
    lv_obj_set_style_border_side(bar, LV_BORDER_SIDE_BOTTOM, sel_on);

    lv_obj_t* t_sys = lv_tabview_add_tab(tabview, "System");
    lv_obj_t* t_audio = lv_tabview_add_tab(tabview, "Audio");
    lv_obj_t* t_link = lv_tabview_add_tab(tabview, "Link");
    lv_obj_t* t_storage = lv_tabview_add_tab(tabview, "Storage");
    lv_obj_t* t_midi = lv_tabview_add_tab(tabview, "MIDI");

    lv_obj_t* tabs[TAB_COUNT] = {t_sys, t_audio, t_link, t_storage, t_midi};
    for (int i = 0; i < TAB_COUNT; i++) {
        lv_obj_set_style_bg_color(tabs[i], lv_color_hex(kColBg), 0);
        lv_obj_set_style_pad_all(tabs[i], 0, 0);
        lv_obj_set_style_border_width(tabs[i], 0, 0);
        lv_obj_remove_flag(tabs[i], LV_OBJ_FLAG_SCROLLABLE);
    }

    buildSystemTab(t_sys);
    buildLinkTab(t_link);
    buildAudioTab(t_audio);
    buildStorageTab(t_storage);
    buildMidiTab(t_midi);

    // The tab bar switches pages by itself. Without this, touching a tab
    // changed the view but left active_tab at TAB_SYSTEM, so the refresh timer
    // kept updating a tab nobody was looking at and the visible one stayed
    // blank - which is exactly how Audio, Link and Storage read as broken.
    lv_obj_add_event_cb(tabview, &UIDiagnosticsPage::onTabChanged, LV_EVENT_VALUE_CHANGED, this);
    lv_tabview_set_active(tabview, active_tab, LV_ANIM_OFF);
}

void UIDiagnosticsPage::onTabChanged(lv_event_t* e) {
    auto* self = static_cast<UIDiagnosticsPage*>(lv_event_get_user_data(e));
    if (!self || !self->tabview) {
        return;
    }
    self->setActiveTab(static_cast<uint8_t>(lv_tabview_get_tab_active(self->tabview)));
}

void UIDiagnosticsPage::setActiveTab(uint8_t tab) {
    if (tab >= TAB_COUNT) {
        return;
    }
    active_tab = tab;
    if (tabview && lv_tabview_get_tab_active(tabview) != tab) {
        lv_tabview_set_active(tabview, tab, LV_ANIM_OFF);
    }
    // Refresh now rather than waiting for the next timer tick: otherwise a
    // freshly-opened tab shows whatever it was built with for up to a second,
    // which reads as "this tab has no data".
    ui_update_pending = true;
    applyUiUpdates();
}

UIDiagnosticsPage::Card UIDiagnosticsPage::makeCard(lv_obj_t* parent,
                                                    int x,
                                                    int y,
                                                    int w,
                                                    const char* title,
                                                    const char* tag,
                                                    bool gauge,
                                                    int warn_pct) {
    Card c = {};
    c.warn_pct = warn_pct;

    lv_obj_t* card = lv_obj_create(parent);
    lv_obj_set_size(card, w, kCardH);
    lv_obj_set_pos(card, x, y);
    lv_obj_set_style_bg_color(card, lv_color_hex(kColCard), 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_border_color(card, lv_color_hex(kColBorder), 0);
    lv_obj_set_style_radius(card, 4, 0);
    lv_obj_set_style_pad_all(card, 0, 0);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    mkLabel(card, 16, 14, title, &lv_font_montserrat_18, kColDim);
    if (tag) {
        // Source tag ("esp32" / "wire" / "new") - which side owns the number.
        uint32_t tc = kColDimmer;
        if (strcmp(tag, "wire") == 0)
            tc = kColBlue;
        else if (strcmp(tag, "new") == 0)
            tc = kColOrange;
        lv_obj_t* l = mkLabel(card, 0, 0, tag, &lv_font_montserrat_14, tc);
        lv_obj_align(l, LV_ALIGN_TOP_RIGHT, -16, 14);
    }

    c.value = mkLabel(card, 16, 64, "-", &lv_font_montserrat_36, 0xFFFFFF);
    c.unit = mkLabel(card, 0, 0, "", &lv_font_montserrat_22, kColDim);
    lv_obj_align_to(c.unit, c.value, LV_ALIGN_OUT_RIGHT_BOTTOM, 10, -6);

    c.sub = mkLabel(card, 16, 112, "", &lv_font_montserrat_18, kColDim);
    lv_obj_set_width(c.sub, w - 32);
    lv_label_set_long_mode(c.sub, LV_LABEL_LONG_WRAP);

    if (gauge) {
        c.bar = lv_bar_create(card);
        lv_obj_set_size(c.bar, kGaugeW, 14);
        lv_obj_set_pos(c.bar, 16, 196);
        lv_bar_set_range(c.bar, 0, 100);
        lv_bar_set_value(c.bar, 0, LV_ANIM_OFF);
        lv_obj_set_style_bg_color(c.bar, lv_color_hex(kColTrack), LV_PART_MAIN);
        lv_obj_set_style_bg_color(c.bar, lv_color_hex(kColGreen), LV_PART_INDICATOR);
        lv_obj_set_style_radius(c.bar, 2, LV_PART_MAIN);
        lv_obj_set_style_radius(c.bar, 2, LV_PART_INDICATOR);
    }
    return c;
}

// Sparkline: an lv_chart line series across the card's mid band. Cheap, and
// per docs/ui-diagnostics-spec.md a trend beats a number - it turns "CPU is
// 40%" into "CPU has been climbing for half a minute".
void UIDiagnosticsPage::addSpark(Card& c, uint32_t colour) {
    if (!c.value) {
        return;
    }
    lv_obj_t* card = lv_obj_get_parent(c.value);
    c.spark = lv_chart_create(card);
    lv_obj_set_size(c.spark, kGaugeW, 52);
    lv_obj_set_pos(c.spark, 16, 118);
    lv_chart_set_type(c.spark, LV_CHART_TYPE_LINE);
    lv_chart_set_point_count(c.spark, kSparkPoints);
    lv_chart_set_range(c.spark, LV_CHART_AXIS_PRIMARY_Y, 0, 100);
    lv_chart_set_div_line_count(c.spark, 0, 0);
    lv_chart_set_update_mode(c.spark, LV_CHART_UPDATE_MODE_SHIFT);
    lv_obj_set_style_bg_color(c.spark, lv_color_hex(0x0C0C0C), LV_PART_MAIN);
    lv_obj_set_style_border_width(c.spark, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(c.spark, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(c.spark, 2, LV_PART_MAIN);
    lv_obj_set_style_size(c.spark, 0, 0, LV_PART_INDICATOR);  // line only, no dots
    lv_obj_set_style_line_width(c.spark, 2, LV_PART_ITEMS);
    c.series = lv_chart_add_series(c.spark, lv_color_hex(colour), LV_CHART_AXIS_PRIMARY_Y);
    // Start flat at zero rather than at LV_CHART_POINT_NONE, so the trace
    // reads as "no load yet" instead of leaving a blank panel that looks
    // like the tile is broken.
    for (uint16_t i = 0; i < kSparkPoints; i++) {
        lv_chart_set_next_value(c.spark, c.series, 0);
    }
}

void UIDiagnosticsPage::addSecondBar(Card& c, uint32_t colour) {
    if (!c.bar) {
        return;
    }
    lv_obj_t* card = lv_obj_get_parent(c.bar);
    // Sits directly under the first, so the two cores read as one pair.
    c.bar2 = lv_bar_create(card);
    lv_obj_set_size(c.bar2, kGaugeW, 14);
    lv_obj_set_pos(c.bar2, 16, 196);
    lv_obj_set_pos(c.bar, 16, 176);
    lv_bar_set_range(c.bar2, 0, 100);
    lv_bar_set_value(c.bar2, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(c.bar2, lv_color_hex(kColTrack), LV_PART_MAIN);
    lv_obj_set_style_bg_color(c.bar2, lv_color_hex(colour), LV_PART_INDICATOR);
    lv_obj_set_style_radius(c.bar2, 2, LV_PART_MAIN);
    lv_obj_set_style_radius(c.bar2, 2, LV_PART_INDICATOR);
}

void UIDiagnosticsPage::pushSpark(Card& c, int value) {
    if (!c.spark || !c.series) {
        return;
    }
    if (value < 0)
        value = 0;
    if (value > 100)
        value = 100;
    lv_chart_set_next_value(c.spark, c.series, static_cast<int32_t>(value));
}

void UIDiagnosticsPage::setCard(
    Card& c, const char* value, const char* unit, const char* sub, int pct) {
    if (!c.value)
        return;
    lv_label_set_text(c.value, value);
    lv_label_set_text(c.unit, unit ? unit : "");
    lv_obj_align_to(c.unit, c.value, LV_ALIGN_OUT_RIGHT_BOTTOM, 10, -6);
    lv_label_set_text(c.sub, sub ? sub : "");
    if (c.bar) {
        if (pct < 0)
            pct = 0;
        if (pct > 100)
            pct = 100;
        lv_bar_set_value(c.bar, pct, LV_ANIM_OFF);
        // Orange is a real warning, not decoration: only where high is bad.
        const bool warn = (c.warn_pct > 0 && pct >= c.warn_pct);
        lv_obj_set_style_bg_color(
            c.bar, lv_color_hex(warn ? kColOrange : kColGreen), LV_PART_INDICATOR);
    }
}

void UIDiagnosticsPage::buildSystemTab(lv_obj_t* tab) {
    // Every figure here is ESP32-local, so this tab is fully live today.
    struct Def {
        const char* title;
        bool gauge;
        int warn;
    };
    static const Def defs[8] = {
        {"ESP32 CPU", true, 85},
        {"DAISY CPU", true, 85},
        {"HEAP INTERNAL", true, 85},
        {"PSRAM", true, 85},
        {"LVGL POOL", true, 85},
        {"TASKS", false, 0},
        {"UPTIME", false, 0},
        {"MIN FREE HEAP", false, 0},
    };
    for (int i = 0; i < 8; i++) {
        sys_cards[i] = makeCard(tab,
                                kColX[i % 4],
                                kRowY[i / 4],
                                kCardW,
                                defs[i].title,
                                i == 1 ? "wire" : "esp32",
                                defs[i].gauge,
                                defs[i].warn);
    }
    // ESP32 CPU carries both cores: one sparkline of the busier core plus a
    // bar each. Two near-identical tiles side by side made the pair hard to
    // read and cost a slot the Daisy needed.
    addSpark(sys_cards[0], kColBlue);
    addSecondBar(sys_cards[0], kColBlue);
    // Daisy CPU is the figure that predicts an underrun, so it earns a trend
    // of its own. It comes free from HeartbeatMessage - no protocol change.
    addSpark(sys_cards[1], kColGreen);
}

void UIDiagnosticsPage::buildLinkTab(lv_obj_t* tab) {
    static const char* titles[4] = {"LINK", "FRAMES/s", "PACKETS", "ERRORS"};
    // Daisy CPU moved to the System tab, where it sits beside the ESP32's own
    // and can be compared at a glance. Frames/s takes its place here, derived
    // from the frontend's own packet counter - free, and it needs neither
    // MSG_DIAG_PUSH nor WAVEX_DAISY_UART_PERF_DEBUG.
    static const bool gauge[4] = {false, false, false, false};
    for (int i = 0; i < 4; i++) {
        link_cards[i] =
            makeCard(tab, kColX[i % 2], kRowY[i / 2], kCardW, titles[i], "esp32", gauge[i], 0);
    }
    addSpark(link_cards[1], kColBlue);

    // Per-message-type counts. wavex_packet_stats_t already tracks these, so
    // the table needs no new plumbing at all.
    lv_obj_t* panel = lv_obj_create(tab);
    lv_obj_set_size(panel, 622, 464);
    lv_obj_set_pos(panel, kColX[2], 0);
    lv_obj_set_style_bg_color(panel, lv_color_hex(kColCard), 0);
    lv_obj_set_style_border_width(panel, 1, 0);
    lv_obj_set_style_border_color(panel, lv_color_hex(kColBorder), 0);
    lv_obj_set_style_radius(panel, 4, 0);
    lv_obj_set_style_pad_all(panel, 0, 0);

    msg_table = lv_table_create(panel);
    lv_obj_set_size(msg_table, 620, 462);
    lv_obj_set_pos(msg_table, 0, 0);
    lv_table_set_column_count(msg_table, 2);
    lv_table_set_column_width(msg_table, 0, 430);
    lv_table_set_column_width(msg_table, 1, 180);
    lv_obj_set_style_bg_color(msg_table, lv_color_hex(kColCard), LV_PART_ITEMS);
    lv_obj_set_style_text_color(msg_table, lv_color_white(), LV_PART_ITEMS);
    lv_obj_set_style_text_font(msg_table, &lv_font_montserrat_18, LV_PART_ITEMS);
    lv_obj_set_style_border_color(msg_table, lv_color_hex(0x222222), LV_PART_ITEMS);
    lv_obj_set_style_border_width(msg_table, 1, LV_PART_ITEMS);
    lv_obj_set_style_bg_color(msg_table, lv_color_hex(kColCard), LV_PART_MAIN);
    lv_obj_set_style_border_width(msg_table, 0, LV_PART_MAIN);
}

void UIDiagnosticsPage::buildAudioTab(lv_obj_t* tab) {
    struct Def {
        const char* title;
        bool gauge;
        int warn;
    };
    // Callback rate first: it is the most valuable single number on the page.
    // It separates "the engine stopped" from "the ring starved" - an ambiguity
    // that cost hours, because an underrun is only detectable INSIDE the
    // callback. If the callback stops, the ring stays full and the log stays
    // clean, so silence looks identical to health everywhere else.
    static const Def defs[8] = {
        {"CALLBACK RATE", false, 0},
        {"RING LOW WATER", true, 0},  // low is bad; coloured explicitly below
        {"UNDERRUNS", false, 0},
        {"ENGINE CPU", true, 60},
        {"PRE-BUFFER", true, 0},  // a full pre-buffer is the healthy state
        {"CURRENT WAV", false, 0},
        {"RING PUSHES", false, 0},
        {"DISCARDED", false, 0},
    };
    for (int i = 0; i < 8; i++) {
        audio_cards[i] = makeCard(tab,
                                  kColX[i % 4],
                                  kRowY[i / 4],
                                  kCardW,
                                  defs[i].title,
                                  "wire",
                                  defs[i].gauge,
                                  defs[i].warn);
    }
}

void UIDiagnosticsPage::buildStorageTab(lv_obj_t* tab) {
    struct Def {
        const char* title;
        bool gauge;
        int warn;
    };
    static const Def defs[8] = {
        {"CARD", false, 0},
        {"THROUGHPUT", false, 0},
        {"READ LATENCY", false, 0},
        {"ERRORS", false, 0},
        {"LAST RESULT", false, 0},
        {"SAMPLE RAM", true, 85},
        {"LARGEST BLOCK", false, 0},
        {"SAMPLES", false, 0},
    };
    for (int i = 0; i < 8; i++) {
        storage_cards[i] = makeCard(tab,
                                    kColX[i % 4],
                                    kRowY[i / 4],
                                    kCardW,
                                    defs[i].title,
                                    "wire",
                                    defs[i].gauge,
                                    defs[i].warn);
    }
}

void UIDiagnosticsPage::buildMidiTab(lv_obj_t* tab) {
    static const char* titles[4] = {"SYNC STATE", "TEMPO", "NOTES", "CONTROL CHANGE"};
    for (int i = 0; i < 4; i++) {
        midi_cards[i] = makeCard(tab, kColX[i], kRowY[0], kCardW, titles[i], "wire", false, 0);
    }
    // These fields exist on the wire but nothing fills them yet: the sequencer
    // and tempo follower are Phase 2. Saying so beats four cards reading zero
    // with no explanation, which looks like a fault rather than a gap.
    midi_note = mkLabel(tab,
                        kColX[0],
                        kRowY[1] + 20,
                        "MSG_DIAG_PUSH carries these fields, but the Daisy has no sequencer or\n"
                        "tempo follower to fill them yet (Phase 2). They will read zero until\n"
                        "those land - see docs/roadmap.md.",
                        &lv_font_montserrat_18,
                        kColDimmer);
}

void UIDiagnosticsPage::showTabOffline(Card* cards, int n, const char* why) {
    for (int i = 0; i < n; i++) {
        setCard(cards[i], "-", "", why, -1);
    }
}

void UIDiagnosticsPage::onExit() {
    ESP_LOGI(TAG, "Diagnostics page exiting");

    // Stop diagnostics monitoring
    inter_mcu_send_diag_subscribe(false, 2);
    stopDiagnosticsMonitoring();

    // Clear UI element references
    ui_update_pending = false;
}

void UIDiagnosticsPage::onInput(const InputEvent& evt) {
    // Handle input events if needed
    // For now, diagnostics page is read-only
}

std::array<Softkey, NUM_SOFTKEYS> UIDiagnosticsPage::getSoftkeys() {
    std::array<Softkey, NUM_SOFTKEYS> keys{};

    keys[0] = {"Back", [this]() { UINavigator::instance().pop(); }};

    // Tab </> move the active tab. The tab bar itself stays out of the encoder
    // focus ring, so tabs are reachable without an extra focus mode.
    keys[1] = {"Tab <", [this]() { setActiveTab((active_tab + TAB_COUNT - 1) % TAB_COUNT); }};
    keys[2] = {"Tab >", [this]() { setActiveTab((active_tab + 1) % TAB_COUNT); }};
    // Freeze: values often change faster than they can be read.
    keys[3] = {"Freeze", [this]() {
                   frozen = !frozen;
                   UINavigator::instance().refreshSoftkeys();
               }};
    keys[5] = {"Samples", []() { UINavigator::instance().push(createSampleMemoryPage()); }};

    if (frozen) {
        keys[3].label = "Live";
    }
    return keys;
}

void UIDiagnosticsPage::startDiagnosticsMonitoring() {
    // Create ESP timer for data collection
    const esp_timer_create_args_t diag_timer_args = {
        .callback = &UIDiagnosticsPage::diagnosticsUpdateCallback,
        .arg = this,
        .name = "diagnostics_timer"};

    esp_err_t timer_ret = esp_timer_create(&diag_timer_args, &diagnostics_timer_handle);
    if (timer_ret == ESP_OK) {
        esp_timer_start_periodic(diagnostics_timer_handle, 500000);  // 500ms
        ESP_LOGI(TAG, "Diagnostics timer started");
    } else {
        ESP_LOGE(TAG, "Failed to create diagnostics timer: %s", esp_err_to_name(timer_ret));
    }

    // Create LVGL timer for UI updates
    lvgl_update_timer = lv_timer_create(lvglUpdateTimerCallback, 50, this);  // Check every 50ms
    if (!lvgl_update_timer) {
        ESP_LOGE(TAG, "Failed to create LVGL update timer");
    }
}

void UIDiagnosticsPage::stopDiagnosticsMonitoring() {
    // Stop and delete LVGL timer first
    if (lvgl_update_timer) {
        lv_timer_del(lvgl_update_timer);
        lvgl_update_timer = nullptr;
    }

    // Stop and delete ESP timer
    if (diagnostics_timer_handle) {
        esp_timer_stop(diagnostics_timer_handle);
        esp_timer_delete(diagnostics_timer_handle);
        diagnostics_timer_handle = nullptr;
    }

    ESP_LOGI(TAG, "Diagnostics monitoring stopped");
}

void UIDiagnosticsPage::updateCpuUsage() {
#if WAVEX_CPU_USAGE_METHOD == 1
    updateCpuUsageFreertosStats();
#elif WAVEX_CPU_USAGE_METHOD == 2
    updateCpuUsageEspIdfBuiltin();
#else
#error "Invalid WAVEX_CPU_USAGE_METHOD value. Must be 1 or 2."
#endif
}

// Upper bound for uxTaskGetSystemState(). Comfortably above the ~15 tasks this
// firmware runs; tasks beyond it are simply not sampled, which skews the total
// rather than corrupting anything.
static constexpr UBaseType_t kMaxTrackedTasks = 32;

void UIDiagnosticsPage::updateCpuUsageFreertosStats() {
    uint32_t current_time_ms = (uint32_t)(esp_timer_get_time() / 1000);

    if (last_check_time_ms == 0) {
        // Initialize previous runtime counters
        last_total_runtime = 0;
        last_idle_runtime_core0 = 0;
        last_idle_runtime_core1 = 0;
        last_check_time_ms = current_time_ms;
        return;
    }

    uint32_t time_diff = current_time_ms - last_check_time_ms;
    if (time_diff >= 2000) {  // Update every 2 seconds (reduce frequency to avoid watchdog timeout)
        // uxTaskGetSystemState() rather than vTaskGetRunTimeStats(): the
        // latter formats every task into a 2 KB heap buffer that this function
        // then strtok-parsed straight back into numbers. Asking for the numbers
        // directly drops the allocation, the formatting and the reparse - which
        // matters because this runs in the shared esp_timer task, the same one
        // that delivers LVGL's tick, so whatever it spends here delays frames.
        //
        // pcTaskName here is the real task name. The old path had to strip
        // trailing spaces because vTaskGetRunTimeStats pads names to a fixed
        // width; do not reintroduce that trimming, it would corrupt any name
        // that legitimately ends in a space.
        static TaskStatus_t task_status[kMaxTrackedTasks];
        uint32_t sampled_total = 0;
        const UBaseType_t task_count =
            uxTaskGetSystemState(task_status, kMaxTrackedTasks, &sampled_total);

        uint32_t idle_runtime_core0 = 0;
        uint32_t idle_runtime_core1 = 0;
        uint32_t total_system_runtime = 0;
        for (UBaseType_t i = 0; i < task_count; i++) {
            const uint32_t runtime = static_cast<uint32_t>(task_status[i].ulRunTimeCounter);
            total_system_runtime += runtime;
            const char* task_name = task_status[i].pcTaskName;
            if (strcmp(task_name, "IDLE0") == 0) {
                idle_runtime_core0 = runtime;
            } else if (strcmp(task_name, "IDLE1") == 0) {
                idle_runtime_core1 = runtime;
            }
        }

        // Calculate per-core CPU usage using idle time
        // CPU usage = (total_time - idle_time) / total_time * 100

        if (total_system_runtime > last_total_runtime && last_total_runtime > 0) {
            // Calculate idle time differences
            uint32_t idle_diff_core0 = idle_runtime_core0 - last_idle_runtime_core0;
            uint32_t idle_diff_core1 = idle_runtime_core1 - last_idle_runtime_core1;
            uint32_t total_diff = total_system_runtime - last_total_runtime;

            // CPU usage calculation based on IDLE task runtime
            // CPU% = 100 - (idle_ticks / total_ticks) * 100
            if (total_diff > 0) {
                // total_diff accumulates BOTH cores, so a single core's share
                // of wall-clock time is half of it. Dividing one core's idle
                // by the two-core total (as this did) halves every idle
                // fraction, pinning a fully idle system at 50%.
                const float per_core_diff = (float)total_diff / 2.0f;
                float core0_usage = 100.0f - ((float)idle_diff_core0 / per_core_diff * 100.0f);
                float core1_usage = 100.0f - ((float)idle_diff_core1 / per_core_diff * 100.0f);

                // Overall CPU usage (average of both cores)
                cpu_usage_percent = (core0_usage + core1_usage) / 2.0f;

                // Update rolling average for stability
                cpu_usage_history[cpu_measurement_count % 10] = cpu_usage_percent;
                cpu_measurement_count++;

                float sum = 0.0f;
                int count = std::min(10, (int)cpu_measurement_count);
                for (int i = 0; i < count; i++) {
                    sum += cpu_usage_history[i];
                }
                cpu_usage_percent = sum / count;

                // Set per-core values
                cpu_usage_core0 = core0_usage;
                cpu_usage_core1 = core1_usage;

                // Clamp values to reasonable ranges
                cpu_usage_percent = std::max(0.0f, std::min(100.0f, cpu_usage_percent));
                cpu_usage_core0 = std::max(0.0f, std::min(100.0f, cpu_usage_core0));
                cpu_usage_core1 = std::max(0.0f, std::min(100.0f, cpu_usage_core1));

                if (cpu_measurement_count % 30 == 0) {
                    ESP_LOGI(TAG,
                             "FreeRTOS CPU Usage: Total=%.1f%% Core0=%.1f%% Core1=%.1f%%",
                             cpu_usage_percent,
                             cpu_usage_core0,
                             cpu_usage_core1);
                }
            }
        }

        // Store values for next iteration
        last_total_runtime = total_system_runtime;
        last_idle_runtime_core0 = idle_runtime_core0;
        last_idle_runtime_core1 = idle_runtime_core1;

        last_check_time_ms = current_time_ms;
    }
}

void UIDiagnosticsPage::updateCpuUsageEspIdfBuiltin() {
    uint32_t current_time_ms = (uint32_t)(esp_timer_get_time() / 1000);

    if (last_esp_idf_check_time == 0) {
        last_esp_idf_check_time = current_time_ms;
        return;
    }

    uint32_t time_diff = current_time_ms - last_esp_idf_check_time;
    if (time_diff >= 1000) {  // Update every 1 second
        UBaseType_t task_count = uxTaskGetNumberOfTasks();
        size_t free_heap = esp_get_free_heap_size();
        size_t min_free_heap = esp_get_minimum_free_heap_size();

        // Get task list to analyze task states
        TaskStatus_t* task_status_array = nullptr;
        UBaseType_t task_status_array_size = task_count + 10;  // Extra space
        task_status_array = (TaskStatus_t*)malloc(sizeof(TaskStatus_t) * task_status_array_size);

        uint32_t running_tasks = 0;
        uint32_t blocked_tasks = 0;

        if (task_status_array) {
            UBaseType_t returned_task_count =
                uxTaskGetSystemState(task_status_array, task_status_array_size, nullptr);

            for (UBaseType_t i = 0; i < returned_task_count; i++) {
                eTaskState state = task_status_array[i].eCurrentState;
                if (state == eRunning || state == eReady) {
                    running_tasks++;
                } else if (state == eBlocked) {
                    blocked_tasks++;
                }
            }

            free(task_status_array);
        }

        // Calculate CPU usage based on system load indicators
        float base_load = 2.0f;  // Base system overhead

        // Task-based load (more tasks = more CPU usage)
        float task_load = (float)task_count * 0.2f;

        // Running task load (tasks that are actually executing)
        float running_load = (float)running_tasks * 1.5f;

        // Memory pressure load (low memory = more CPU usage from GC/compaction)
        float memory_load = 0.0f;
        if (free_heap < 30000) {
            memory_load = 25.0f;  // High memory pressure
        } else if (free_heap < 60000) {
            memory_load = 15.0f;  // Medium memory pressure
        } else if (free_heap < 100000) {
            memory_load = 8.0f;  // Light memory pressure
        }

        cpu_usage_percent = base_load + task_load + running_load + memory_load;

        // For ESP32-P4 (dual core), distribute usage across cores
        cpu_usage_core0 = cpu_usage_percent * 0.6f;  // Approximate core 0 usage
        cpu_usage_core1 = cpu_usage_percent * 0.4f;  // Approximate core 1 usage

        // Clamp values to reasonable ranges
        cpu_usage_percent = std::max(0.0f, std::min(100.0f, cpu_usage_percent));
        cpu_usage_core0 = std::max(0.0f, std::min(100.0f, cpu_usage_core0));
        cpu_usage_core1 = std::max(0.0f, std::min(100.0f, cpu_usage_core1));

        // Update rolling average for stability
        cpu_usage_history[cpu_measurement_count % 10] = cpu_usage_percent;
        cpu_measurement_count++;

        float sum = 0.0f;
        int count = std::min(10, (int)cpu_measurement_count);
        for (int i = 0; i < count; i++) {
            sum += cpu_usage_history[i];
        }
        cpu_usage_percent = sum / count;

        if (cpu_measurement_count % 10 == 0) {
            ESP_LOGI(TAG,
                     "ESP-IDF CPU Usage: %.1f%% (Tasks: %d running/%d total, Heap: %zu/%zu KB)",
                     cpu_usage_percent,
                     running_tasks,
                     task_count,
                     free_heap / 1024,
                     min_free_heap / 1024);
        }

        last_esp_idf_check_time = current_time_ms;
    }
}

// Timer context: sample the CPU counters (which must be read at a steady
// cadence to be meaningful) and flag the UI. Every other figure is read
// straight from its source in the per-tab refresh, on the UI task.
void UIDiagnosticsPage::collectDiagnosticsData() {
    updateCpuUsage();
    ui_update_pending = true;
    wavex_ui_mark_content_changed();
}

void UIDiagnosticsPage::applyUiUpdates() {
    if (!ui_update_pending) {
        return;
    }
    ui_update_pending = false;
    if (frozen) {
        return;  // hold the last values so a transient can be read
    }

    switch (active_tab) {
        case TAB_SYSTEM:
            refreshSystemTab();
            break;
        case TAB_LINK:
            refreshLinkTab();
            break;
        case TAB_AUDIO:
            refreshAudioTab();
            break;
        case TAB_STORAGE:
            refreshStorageTab();
            break;
        case TAB_MIDI:
            refreshMidiTab();
            break;
        default:
            break;
    }
}

namespace {
// Names for DiagPushMessage::sd_speed_index. The Daisy sends the index and
// keeps the table, so adding a speed there needs no protocol change - but the
// two lists must stay in step, hence the explicit unknown fallback.
const char* SdSpeedName(uint8_t index) {
    static const char* kNames[] = {
        "SLOW 400kHz", "MEDIUM 12.5MHz", "STANDARD 25MHz", "FAST 50MHz", "VERY FAST 100MHz"};
    return index < (sizeof(kNames) / sizeof(kNames[0])) ? kNames[index] : "unknown";
}

const char* SyncStateName(uint8_t state) {
    switch (state) {
        case 0:
            return "INTERNAL";
        case 1:
            return "ACQUIRING";
        case 2:
            return "LOCKED";
        case 3:
            return "FREEWHEEL";
        default:
            return "?";
    }
}

// FatFS FRESULT names, for the ones a streaming read can actually produce.
const char* FatFsName(uint8_t fr) {
    switch (fr) {
        case 0:
            return "FR_OK";
        case 1:
            return "FR_DISK_ERR";
        case 2:
            return "FR_INT_ERR";
        case 3:
            return "FR_NOT_READY";
        case 4:
            return "FR_NO_FILE";
        case 9:
            return "FR_INVALID_OBJECT";
        case 13:
            return "FR_NO_FILESYSTEM";
        default:
            return "FRESULT";
    }
}
}  // namespace

// Telemetry older than this is not shown. At the 2 Hz subscription rate three
// missed pushes means the backend or the link is in trouble, and a frozen
// figure presented as current is exactly how a dead link reads as a healthy
// one - the failure mode this page exists to catch.
static constexpr uint32_t kDiagMaxAgeMs = 1500;

void UIDiagnosticsPage::refreshAudioTab() {
    if (!audio_cards[0].value || !lv_obj_is_valid(audio_cards[0].value)) {
        return;
    }
    WaveX::Protocol::DiagPushMessage d;
    if (!inter_mcu_get_diag_push(&d, kDiagMaxAgeMs)) {
        showTabOffline(audio_cards, 8, "no telemetry from backend");
        return;
    }
    char v[48], u[32], sub[80];

    snprintf(v, sizeof(v), "%u.%u", d.callback_hz_x10 / 10, d.callback_hz_x10 % 10);
    // Below ~990 Hz the engine is missing blocks; that is a stopped or
    // starved callback, not jitter.
    const bool cb_bad = d.callback_hz_x10 < 9900;
    setCard(audio_cards[0], v, "Hz", cb_bad ? "BELOW NOMINAL 1000 Hz" : "nominal 1000 Hz", -1);

    snprintf(v, sizeof(v), "%u", d.ring_low_water);
    snprintf(u, sizeof(u), "/ 2048");
    const int ring_pct = (d.ring_low_water * 100) / 2048;
    // Inverted on purpose: on this gauge LOW is the problem, so the warning
    // threshold is expressed against the empty end.
    snprintf(sub, sizeof(sub), "%s", ring_pct < 20 ? "STARVING" : "lowest this interval");
    setCard(audio_cards[1], v, u, sub, ring_pct);
    if (audio_cards[1].bar) {
        lv_obj_set_style_bg_color(audio_cards[1].bar,
                                  lv_color_hex(ring_pct < 20 ? kColOrange : kColGreen),
                                  LV_PART_INDICATOR);
    }

    snprintf(v, sizeof(v), "%u", d.underruns);
    snprintf(sub, sizeof(sub), "episodes in %lu ms", (unsigned long)d.interval_ms);
    setCard(audio_cards[2], v, "", sub, -1);

    snprintf(v, sizeof(v), "%u.%u", d.engine_cpu_x10 / 10, d.engine_cpu_x10 % 10);
    snprintf(sub, sizeof(sub), "max %u.%u%%", d.engine_cpu_max_x10 / 10, d.engine_cpu_max_x10 % 10);
    setCard(audio_cards[3], v, "%", sub, d.engine_cpu_x10 / 10);

    snprintf(v, sizeof(v), "%u", d.prebuffer_filled);
    setCard(audio_cards[4], v, "/ 1024", "full is healthy", (d.prebuffer_filled * 100) / 1024);

    if (d.wav_sample_rate) {
        snprintf(v, sizeof(v), "%lu", (unsigned long)(d.wav_sample_rate / 1000));
        snprintf(sub,
                 sizeof(sub),
                 "%u-bit %s%s",
                 d.wav_bits,
                 d.wav_channels == 2 ? "stereo" : "mono",
                 d.resampling ? " - RESAMPLING" : "");
        setCard(audio_cards[5], v, "kHz", sub, -1);
    } else {
        setCard(audio_cards[5], "-", "", d.playing ? "playing, no format yet" : "not playing", -1);
    }

    snprintf(v, sizeof(v), "%lu", (unsigned long)d.ring_pushes);
    setCard(audio_cards[6], v, "", "this interval", -1);

    // Its own card because skip-without-consume is invisible in every other
    // figure: the ring simply stops filling, and both playback stalls began
    // exactly this way.
    snprintf(v, sizeof(v), "%lu", (unsigned long)d.ring_discards);
    setCard(audio_cards[7],
            v,
            "",
            d.ring_discards ? "SKIPPED WITHOUT CONSUMING" : "passes skipped",
            -1);
}

void UIDiagnosticsPage::refreshStorageTab() {
    if (!storage_cards[0].value || !lv_obj_is_valid(storage_cards[0].value)) {
        return;
    }
    WaveX::Protocol::DiagPushMessage d;
    if (!inter_mcu_get_diag_push(&d, kDiagMaxAgeMs)) {
        showTabOffline(storage_cards, 8, "no telemetry from backend");
        return;
    }
    char v[48], sub[80];

    setCard(storage_cards[0],
            d.sd_mounted ? "MOUNTED" : "ABSENT",
            "",
            d.sd_mounted ? SdSpeedName(d.sd_speed_index) : "no card",
            -1);

    if (d.interval_ms) {
        const uint32_t kbps =
            (uint32_t)(((uint64_t)d.sd_bytes * 1000ull) / d.interval_ms / 1024ull);
        snprintf(v, sizeof(v), "%lu", (unsigned long)kbps);
        snprintf(sub, sizeof(sub), "%u reads this interval", d.sd_reads);
        setCard(storage_cards[1], v, "KB/s", sub, -1);
    }

    snprintf(v, sizeof(v), "%u.%u", d.sd_lat_avg_us / 1000, (d.sd_lat_avg_us % 1000) / 100);
    // Max belongs on the face of the card, not in a detail row: latency
    // creeping before any error appears is the marginal-timing tell.
    snprintf(
        sub, sizeof(sub), "max %u.%u ms", d.sd_lat_max_us / 1000, (d.sd_lat_max_us % 1000) / 100);
    setCard(storage_cards[2], v, "ms avg", sub, -1);

    snprintf(v, sizeof(v), "%u", d.sd_errors);
    snprintf(sub, sizeof(sub), "%u recoveries", d.sd_recoveries);
    setCard(storage_cards[3], v, "", sub, -1);

    // FRESULT and HAL error together: FR_DISK_ERR alone says only "the read
    // failed", where SDMMC_ERROR_DATA_CRC_FAIL with the card in TRANSFER
    // state says "card healthy, wiring marginal" - a different action.
    snprintf(sub, sizeof(sub), "HAL 0x%08lx", (unsigned long)d.sd_hal_err);
    setCard(storage_cards[4], FatFsName(d.sd_last_fatfs), "", sub, -1);

    const uint32_t ram_free_kb = d.sample_ram_free / 1024;
    snprintf(v, sizeof(v), "%lu", (unsigned long)(ram_free_kb / 1024));
    snprintf(sub, sizeof(sub), "%lu KB free", (unsigned long)ram_free_kb);
    setCard(storage_cards[5], v, "MB free", sub, -1);

    snprintf(v, sizeof(v), "%lu", (unsigned long)(d.sample_ram_largest / 1024));
    setCard(storage_cards[6], v, "KB", "largest contiguous block", -1);

    snprintf(v, sizeof(v), "%u", d.sample_count);
    snprintf(sub, sizeof(sub), "%u failed allocs", d.sample_failed_allocs);
    setCard(storage_cards[7], v, "loaded", sub, -1);
}

void UIDiagnosticsPage::refreshMidiTab() {
    if (!midi_cards[0].value || !lv_obj_is_valid(midi_cards[0].value)) {
        return;
    }
    WaveX::Protocol::DiagPushMessage d;
    if (!inter_mcu_get_diag_push(&d, kDiagMaxAgeMs)) {
        showTabOffline(midi_cards, 4, "no telemetry from backend");
        return;
    }
    char v[48], sub[64];

    setCard(midi_cards[0],
            SyncStateName(d.sync_state),
            "",
            d.transport_playing ? "transport playing" : "transport stopped",
            -1);

    snprintf(v, sizeof(v), "%u.%02u", d.measured_bpm_x100 / 100, d.measured_bpm_x100 % 100);
    snprintf(sub, sizeof(sub), "%u clock ticks", d.midi_clock_ticks);
    setCard(midi_cards[1], v, "BPM", sub, -1);

    snprintf(v, sizeof(v), "%u", d.midi_notes);
    setCard(midi_cards[2], v, "", "this interval", -1);

    snprintf(v, sizeof(v), "%u", d.midi_ccs);
    setCard(midi_cards[3], v, "", "this interval", -1);
}

void UIDiagnosticsPage::refreshSystemTab() {
    if (!sys_cards[0].value || !lv_obj_is_valid(sys_cards[0].value)) {
        return;
    }
    char v[48], u[32], sub[64];

    // The tile shows the busier core, because that is the one that will run
    // out first; both are still visible as separate bars underneath.
    const float busier = cpu_usage_core0 > cpu_usage_core1 ? cpu_usage_core0 : cpu_usage_core1;
    snprintf(v, sizeof(v), "%.1f", busier);
    snprintf(sub, sizeof(sub), "core0 %.1f%%  core1 %.1f%%", cpu_usage_core0, cpu_usage_core1);
    setCard(sys_cards[0], v, "%", sub, (int)cpu_usage_core0);
    if (sys_cards[0].bar2) {
        lv_bar_set_value(sys_cards[0].bar2, (int)cpu_usage_core1, LV_ANIM_OFF);
    }
    pushSpark(sys_cards[0], (int)busier);

    wavex_backend_heartbeat_t hb_sys;
    inter_mcu_get_backend_heartbeat_detailed(&hb_sys);
    if (hb_sys.valid) {
        snprintf(v, sizeof(v), "%.1f", hb_sys.cpu_avg_percent);
        snprintf(sub,
                 sizeof(sub),
                 "min %.1f%%  max %.1f%%",
                 hb_sys.cpu_min_percent,
                 hb_sys.cpu_max_percent);
        setCard(sys_cards[1], v, "%", sub, (int)hb_sys.cpu_avg_percent);
        pushSpark(sys_cards[1], (int)hb_sys.cpu_avg_percent);
    } else {
        setCard(sys_cards[1], "-", "%", "no heartbeat from backend", 0);
    }

    const size_t heap_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    const size_t heap_total = heap_caps_get_total_size(MALLOC_CAP_INTERNAL);
    snprintf(v, sizeof(v), "%u", (unsigned)(heap_free / 1024));
    snprintf(sub, sizeof(sub), "of %u KB", (unsigned)(heap_total / 1024));
    setCard(sys_cards[2],
            v,
            "KB free",
            sub,
            heap_total ? (int)(100 - (heap_free * 100) / heap_total) : 0);

    const size_t ps_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    const size_t ps_total = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
    snprintf(v, sizeof(v), "%.1f", ps_free / (1024.0 * 1024.0));
    snprintf(sub, sizeof(sub), "of %u MB", (unsigned)(ps_total / (1024 * 1024)));
    setCard(
        sys_cards[3], v, "MB free", sub, ps_total ? (int)(100 - (ps_free * 100) / ps_total) : 0);

    lv_mem_monitor_t mon;
    lv_mem_monitor(&mon);
    snprintf(v, sizeof(v), "%u", (unsigned)((mon.total_size - mon.free_size) / 1024));
    snprintf(u, sizeof(u), "/ %u KB", (unsigned)(mon.total_size / 1024));
    snprintf(sub, sizeof(sub), "fragmentation %u%%", (unsigned)mon.frag_pct);
    setCard(sys_cards[4], v, u, sub, (int)mon.used_pct);

    snprintf(v, sizeof(v), "%u", (unsigned)uxTaskGetNumberOfTasks());
    setCard(sys_cards[5], v, "running", "", -1);

    const uint32_t up_s = (uint32_t)(esp_timer_get_time() / 1000000);
    snprintf(v,
             sizeof(v),
             "%luh %02lum",
             (unsigned long)(up_s / 3600),
             (unsigned long)((up_s % 3600) / 60));
    setCard(sys_cards[6], v, "", "", -1);

    snprintf(v, sizeof(v), "%u", (unsigned)(esp_get_minimum_free_heap_size() / 1024));
    setCard(sys_cards[7], v, "KB", "lowest since boot", -1);
}

void UIDiagnosticsPage::refreshLinkTab() {
    if (!link_cards[0].value || !lv_obj_is_valid(link_cards[0].value)) {
        return;
    }
    wavex_packet_stats_t st;
    inter_mcu_get_packet_stats(&st);
    wavex_backend_heartbeat_t hb;
    inter_mcu_get_backend_heartbeat_detailed(&hb);

    const uint32_t uptime_ms = (uint32_t)(esp_timer_get_time() / 1000);
    char v[48], sub[64];

    uint32_t age_ms = 0;
    const char* state = "NO LINK";
    if (hb.valid && hb.last_rx_ms > 0) {
        age_ms = uptime_ms - hb.last_rx_ms;
        state = (age_ms < 2000) ? "OK" : (age_ms < 5000 ? "STALE" : "LOST");
    }
    snprintf(sub,
             sizeof(sub),
             "heartbeat %lu.%lu s ago",
             (unsigned long)(age_ms / 1000),
             (unsigned long)((age_ms % 1000) / 100));
    setCard(link_cards[0], state, "", hb.valid ? sub : "no heartbeat seen", -1);

    if (hb.valid) {
        snprintf(v, sizeof(v), "%.1f", hb.cpu_avg_percent);
        snprintf(sub, sizeof(sub), "min %.1f - max %.1f", hb.cpu_min_percent, hb.cpu_max_percent);
        setCard(link_cards[1], v, "%", sub, (int)hb.cpu_avg_percent);
    } else {
        setCard(link_cards[1], "-", "%", "no heartbeat", 0);
    }

    snprintf(v, sizeof(v), "%lu", (unsigned long)st.total_packets);
    snprintf(sub,
             sizeof(sub),
             "%lu meter - %lu wave",
             (unsigned long)st.meter_push_packets,
             (unsigned long)st.wave_chunk_packets);
    setCard(link_cards[2], v, "total", sub, -1);

    snprintf(v, sizeof(v), "%lu", (unsigned long)(st.invalid_packets + st.error_packets));
    snprintf(sub,
             sizeof(sub),
             "%lu invalid - %lu error - %lu unknown",
             (unsigned long)st.invalid_packets,
             (unsigned long)st.error_packets,
             (unsigned long)st.unknown_packets);
    setCard(link_cards[3], v, "bad", sub, -1);

    if (!msg_table || !lv_obj_is_valid(msg_table)) {
        return;
    }
    struct Row {
        const char* name;
        uint32_t count;
    };
    const Row rows[] = {
        {"HEARTBEAT", st.heartbeat_packets},
        {"METER_PUSH", st.meter_push_packets},
        {"WAVE_CHUNK", st.wave_chunk_packets},
        {"ENVELOPE_CHUNK", st.envelope_chunk_packets},
        {"STATUS_RESPONSE", st.status_response_packets},
        {"STATUS_REQUEST", st.status_request_packets},
        {"SAMPLE_CTRL", st.sample_ctrl_packets},
        {"SAMPLE_LOAD", st.sample_load_packets},
        {"SAMPLE_DATA", st.sample_data_packets},
        {"PREVIEW_REQ", st.preview_req_packets},
        {"DATA_REQUEST", st.data_request_packets},
        {"CONTROL_CHANGE", st.control_change_packets},
        {"NOTE_ON", st.note_on_packets},
        {"NOTE_OFF", st.note_off_packets},
        {"PARAMETER_UPDATE", st.parameter_update_packets},
        {"SYNC", st.sync_packets},
        {"ERROR", st.error_packets},
        {"OTHER (known)", st.other_known_packets},
        {"UNKNOWN", st.unknown_packets},
        {"INVALID", st.invalid_packets},
        {"TOTAL", st.total_packets},
    };
    const int n = (int)(sizeof(rows) / sizeof(rows[0]));
    lv_table_set_row_count(msg_table, n + 1);
    lv_table_set_cell_value(msg_table, 0, 0, "MESSAGE");
    lv_table_set_cell_value(msg_table, 0, 1, "COUNT");
    for (int i = 0; i < n; i++) {
        char cnt[24];
        snprintf(cnt, sizeof(cnt), "%lu", (unsigned long)rows[i].count);
        lv_table_set_cell_value(msg_table, i + 1, 0, rows[i].name);
        lv_table_set_cell_value(msg_table, i + 1, 1, cnt);
    }
}

void UIDiagnosticsPage::diagnosticsUpdateCallback(void* arg) {
    UIDiagnosticsPage* page = (UIDiagnosticsPage*)arg;
    if (page) {
        page->collectDiagnosticsData();
    }
}

void UIDiagnosticsPage::lvglUpdateTimerCallback(lv_timer_t* timer) {
    UIDiagnosticsPage* page = static_cast<UIDiagnosticsPage*>(lv_timer_get_user_data(timer));
    if (page) {
        page->applyUiUpdates();
    }
}

std::shared_ptr<UIPage> createDiagnosticsPage() {
    return std::make_shared<UIDiagnosticsPage>();
}

}  // namespace wavex_ui
