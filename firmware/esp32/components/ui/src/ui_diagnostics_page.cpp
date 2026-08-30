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
#include "ui/input_dispatcher.h"
#include "ui/ui_navigator.h"
#include "ui/ui_palette.h"
#include "ui/ui_tab_group.h"
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
      diagnostics_timer_handle(nullptr),
      lvgl_update_timer(nullptr),
      tabview(nullptr),
      active_tab(TAB_ESP32),
      frozen(false),
      midi_note(nullptr),
      msg_table(nullptr),
      sample_table(nullptr),
      sample_req_ms(0),
      frames_ref_ms(0),
      frames_ref_pkts(0),
      frames_per_s(0),
      ui_update_pending(false) {
    resetUiState();
    memset(cpu_usage_history, 0, sizeof(cpu_usage_history));
}

// Everything that points at an LVGL object, in one place. onEnter() rebuilds
// the whole page from a cleaned parent, so every one of these is dangling by
// then; forgetting one is how a refresh writes through a freed pointer.
void UIDiagnosticsPage::resetUiState() {
    memset(tab_body, 0, sizeof(tab_body));
    memset(tab_built, 0, sizeof(tab_built));
    memset(esp32_cards, 0, sizeof(esp32_cards));
    memset(daisy_cards, 0, sizeof(daisy_cards));
    memset(link_cards, 0, sizeof(link_cards));
    memset(audio_cards, 0, sizeof(audio_cards));
    memset(storage_cards, 0, sizeof(storage_cards));
    memset(midi_cards, 0, sizeof(midi_cards));
    tabview = nullptr;
    midi_note = nullptr;
    msg_table = nullptr;
    sample_table = nullptr;
}

UIDiagnosticsPage::~UIDiagnosticsPage() {
    // Cleanup is done in onExit()
}

const char* UIDiagnosticsPage::name() const {
    return "Diagnostics";
}

namespace {

// Design palette (WaveX Wireframes v2) now lives in ui/ui_palette.h so tab
// groups and cards on other pages match by construction rather than by copy.
using namespace wavex_ui::palette;

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

// Shared card styles.
//
// Every lv_obj_set_style_*() call stores a property in the object's OWN style
// list, which allocates. A card is a container, four labels and often a bar,
// and a tab builds up to eight of them - so the invariant properties were
// ~18 property stores per card, paid again for every card on every tab. They
// are identical across every card on the page, so they belong in one style
// each card references. Same reasoning, and the same lv_obj_remove_style_all()
// step to drop the theme defaults first, as ui_play_page.cpp makeKey().
//
// Statics: initialised once, never reset, which is what an lv_style_t
// referenced by live objects requires. Built and read only on the UI task, so
// the one-time init needs no locking.
lv_style_t s_card;
lv_style_t s_bar_main;
lv_style_t s_bar_ind;
lv_style_t s_title;
lv_style_t s_value;
lv_style_t s_unit;
lv_style_t s_sub;
bool s_styles_ready = false;

void initCardStyles() {
    if (s_styles_ready) {
        return;
    }
    lv_style_init(&s_card);
    // remove_style_all() takes the theme's opaque background with it, so the
    // shared style has to restore the parts a card actually needs.
    lv_style_set_bg_opa(&s_card, LV_OPA_COVER);
    lv_style_set_bg_color(&s_card, lv_color_hex(kColCard));
    lv_style_set_border_width(&s_card, 1);
    lv_style_set_border_color(&s_card, lv_color_hex(kColBorder));
    lv_style_set_border_opa(&s_card, LV_OPA_COVER);
    lv_style_set_radius(&s_card, 4);
    lv_style_set_pad_all(&s_card, 0);

    lv_style_init(&s_bar_main);
    lv_style_set_bg_opa(&s_bar_main, LV_OPA_COVER);
    lv_style_set_bg_color(&s_bar_main, lv_color_hex(kColTrack));
    lv_style_set_radius(&s_bar_main, 2);

    lv_style_init(&s_bar_ind);
    lv_style_set_bg_opa(&s_bar_ind, LV_OPA_COVER);
    lv_style_set_bg_color(&s_bar_ind, lv_color_hex(kColGreen));
    lv_style_set_radius(&s_bar_ind, 2);

    lv_style_init(&s_title);
    lv_style_set_text_font(&s_title, &lv_font_montserrat_18);
    lv_style_set_text_color(&s_title, lv_color_hex(kColDim));

    lv_style_init(&s_value);
    lv_style_set_text_font(&s_value, &lv_font_montserrat_36);
    lv_style_set_text_color(&s_value, lv_color_white());

    lv_style_init(&s_unit);
    lv_style_set_text_font(&s_unit, &lv_font_montserrat_22);
    lv_style_set_text_color(&s_unit, lv_color_hex(kColDim));

    lv_style_init(&s_sub);
    lv_style_set_text_font(&s_sub, &lv_font_montserrat_18);
    lv_style_set_text_color(&s_sub, lv_color_hex(kColDim));

    s_styles_ready = true;
}

// A label carrying one of the shared styles rather than its own copy of a font
// and a colour.
lv_obj_t* mkStyledLabel(lv_obj_t* parent, int x, int y, const char* txt, lv_style_t* style) {
    lv_obj_t* l = lv_label_create(parent);
    lv_obj_remove_style_all(l);
    lv_obj_add_style(l, style, LV_PART_MAIN);
    lv_label_set_text(l, txt);
    lv_obj_set_pos(l, x, y);
    return l;
}

}  // namespace

void UIDiagnosticsPage::onEnter(lv_obj_t* parent) {
    ESP_LOGI(TAG, "Diagnostics page entering");

    lv_obj_clean(parent);
    frozen = false;
    resetUiState();
    // Windowed measurements start fresh: a reference point left over from the
    // last visit would produce one absurd first reading spanning the gap.
    sample_req_ms = 0;
    frames_ref_ms = 0;
    frames_ref_pkts = 0;
    frames_per_s = 0;

    buildTabs(parent);
    startDiagnosticsMonitoring();
    // Subscribe only while this page is open. 2 Hz is ~188 B/s against a
    // 200 KB/s link; closed, the backend sends nothing at all.
    inter_mcu_send_diag_subscribe(true, 2);
}

void UIDiagnosticsPage::buildTabs(lv_obj_t* parent) {
    // Chrome comes from ui_tab_group so every tabbed page in the UI matches by
    // construction. This page was where the styling was written; it now
    // consumes the shared helper rather than owning a private copy of it.
    tabview = tabGroupCreate(parent);

    // Order is the one docs/ui-information-architecture.md §1 pins: the two
    // machines first, then the four subsystem views.
    static const char* kTitles[TAB_COUNT] = {"ESP32", "Daisy", "Audio", "Link", "Storage", "MIDI"};
    for (uint8_t i = 0; i < TAB_COUNT; i++) {
        tab_body[i] = tabGroupAddTab(tabview, kTitles[i]);
    }

    // Tab CONTENT is built on first show, not here.
    //
    // Six tabs of eight cards is ~250 LVGL objects, and page entry is this
    // page's entire cost (docs/backlog.md: 30-47 ms, the worst in the UI)
    // because every object is laid out and drawn in the frame the user is
    // waiting on. Five of the six tabs are, at that moment, invisible. Building
    // one tab instead of six is a straight ~6x cut to that frame, and the
    // deferred cost is paid only for tabs somebody actually opens - the common
    // case being that they open Diagnostics to look at exactly one.
    //
    // Built tabs are kept, not torn down on switch away: the sparklines hold
    // 30 s of history that a rebuild would discard, and re-entering a tab must
    // not cost what entering the page costs. Worst case (visit all six) is what
    // page entry cost before; best case is a sixth of it.
    //
    // The tab bar needs the tab objects themselves to exist for its labels, so
    // those are created up front - six empty containers, which is free.

    // The tab bar switches pages by itself. Without this, touching a tab
    // changed the view but left active_tab where it was, so the refresh timer
    // kept updating a tab nobody was looking at and the visible one stayed
    // blank - which is exactly how Audio, Link and Storage read as broken.
    lv_obj_add_event_cb(tabview, &UIDiagnosticsPage::onTabChanged, LV_EVENT_VALUE_CHANGED, this);
    ensureTabBuilt(active_tab);
    lv_tabview_set_active(tabview, active_tab, LV_ANIM_OFF);
}

void UIDiagnosticsPage::ensureTabBuilt(uint8_t tab) {
    if (tab >= TAB_COUNT || tab_built[tab] || !tab_body[tab]) {
        return;
    }
    // Set first: the build functions are pure LVGL construction, but marking
    // afterwards would let a re-entrant tab event build the same tab twice.
    tab_built[tab] = true;
    switch (tab) {
        case TAB_ESP32:
            buildEsp32Tab(tab_body[tab]);
            break;
        case TAB_DAISY:
            buildDaisyTab(tab_body[tab]);
            break;
        case TAB_AUDIO:
            buildAudioTab(tab_body[tab]);
            break;
        case TAB_LINK:
            buildLinkTab(tab_body[tab]);
            break;
        case TAB_STORAGE:
            buildStorageTab(tab_body[tab]);
            break;
        case TAB_MIDI:
            buildMidiTab(tab_body[tab]);
            break;
        default:
            break;
    }
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
    // First visit to this tab pays for its content here rather than at page
    // entry. This runs on the UI task, from a touch event or a softkey.
    ensureTabBuilt(tab);
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

    initCardStyles();

    lv_obj_t* card = lv_obj_create(parent);
    // Drop the theme's default container styling before adding ours. None of it
    // survives visually, so applying it to every card and then overriding it is
    // pure page-entry cost.
    lv_obj_remove_style_all(card);
    lv_obj_add_style(card, &s_card, LV_PART_MAIN);
    lv_obj_set_size(card, w, kCardH);
    lv_obj_set_pos(card, x, y);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    mkStyledLabel(card, 16, 14, title, &s_title);
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

    c.value = mkStyledLabel(card, 16, 64, "-", &s_value);
    c.unit = mkStyledLabel(card, 0, 0, "", &s_unit);
    lv_obj_align_to(c.unit, c.value, LV_ALIGN_OUT_RIGHT_BOTTOM, 10, -6);

    c.sub = mkStyledLabel(card, 16, 112, "", &s_sub);
    lv_obj_set_width(c.sub, w - 32);
    lv_label_set_long_mode(c.sub, LV_LABEL_LONG_WRAP);

    if (gauge) {
        c.bar = lv_bar_create(card);
        lv_obj_remove_style_all(c.bar);
        lv_obj_add_style(c.bar, &s_bar_main, LV_PART_MAIN);
        lv_obj_add_style(c.bar, &s_bar_ind, LV_PART_INDICATOR);
        lv_obj_set_size(c.bar, kGaugeW, 14);
        lv_obj_set_pos(c.bar, 16, 196);
        lv_bar_set_range(c.bar, 0, 100);
        lv_bar_set_value(c.bar, 0, LV_ANIM_OFF);
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
    // Below the sub line (y 112, ~22 px tall) and above the gauge (y 196).
    // It used to start at 118, drawing straight over the sub text, so the
    // context line on every sparkline card was invisible.
    lv_obj_set_size(c.spark, kGaugeW, 48);
    lv_obj_set_pos(c.spark, 16, 140);
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

void UIDiagnosticsPage::buildEsp32Tab(lv_obj_t* tab) {
    // Every figure here is ESP32-local, so this tab is fully live today.
    struct Def {
        const char* title;
        bool gauge;
        int warn;
    };
    // The two cores are separate tiles now. They used to share one, with a
    // single sparkline of whichever was busier - which is exactly the shape
    // that hides an imbalance, and an imbalance between an audio/link core and
    // a UI core is a thing worth seeing. The slot this used to cost the Daisy
    // is no longer contended: the Daisy has its own tab.
    static const Def defs[8] = {
        {"CPU0", true, 85},
        {"CPU1", true, 85},
        {"HEAP INTERNAL", true, 85},
        {"PSRAM", true, 85},
        {"LVGL POOL", true, 85},
        {"TASKS", false, 0},
        {"UPTIME", false, 0},
        {"MIN FREE HEAP", false, 0},
    };
    for (int i = 0; i < 8; i++) {
        esp32_cards[i] = makeCard(tab,
                                  kColX[i % 4],
                                  kRowY[i / 4],
                                  kCardW,
                                  defs[i].title,
                                  "esp32",
                                  defs[i].gauge,
                                  defs[i].warn);
    }
    addSpark(esp32_cards[0], kColBlue);
    addSpark(esp32_cards[1], kColBlue);
}

void UIDiagnosticsPage::buildDaisyTab(lv_obj_t* tab) {
    struct Def {
        const char* title;
        const char* tag;
        bool gauge;
        int warn;
    };
    // Top row: the backend's CPU and the sample-RAM pool breakdown that the
    // standalone Sample Memory page used to render as a block of text.
    // Bottom row: system heap and uptime, then the resident-sample list.
    //
    // DAISY HEAP is the backend's newlib heap, which is a different allocator
    // from the SDRAM sample pools next to it and fails in a different way -
    // that is why it gets a card rather than being folded into them.
    static const Def defs[6] = {
        {"DAISY CPU", "wire", true, 85},
        {"SMALL POOL", "wire", true, 85},
        {"LARGE POOL", "wire", true, 85},
        {"LARGEST BLOCK", "wire", false, 0},
        {"DAISY HEAP", "wire", true, 85},
        {"DAISY UPTIME", "wire", false, 0},
    };
    for (int i = 0; i < 6; i++) {
        daisy_cards[i] = makeCard(tab,
                                  kColX[i % 4],
                                  kRowY[i / 4],
                                  kCardW,
                                  defs[i].title,
                                  defs[i].tag,
                                  defs[i].gauge,
                                  defs[i].warn);
    }
    // Daisy CPU is the figure that predicts an underrun, so it earns a trend
    // of its own. It comes free from HeartbeatMessage - no protocol change.
    addSpark(daisy_cards[0], kColGreen);

    // Resident samples, from SampleMemStatusMessage. This is the substance of
    // the former Sample Memory page: a table rather than the block of
    // snprintf'd text it used, because eight rows of six fields is a table.
    lv_obj_t* panel = lv_obj_create(tab);
    lv_obj_remove_style_all(panel);
    lv_obj_add_style(panel, &s_card, LV_PART_MAIN);
    lv_obj_set_size(panel, 622, kCardH);
    lv_obj_set_pos(panel, kColX[2], kRowY[1]);
    lv_obj_remove_flag(panel, LV_OBJ_FLAG_SCROLLABLE);

    mkStyledLabel(panel, 16, 10, "RESIDENT SAMPLES", &s_title);

    sample_table = lv_table_create(panel);
    lv_obj_set_size(sample_table, 620, kCardH - 42);
    lv_obj_set_pos(sample_table, 0, 40);
    lv_table_set_column_count(sample_table, 5);
    lv_table_set_column_width(sample_table, 0, 60);   // ID
    lv_table_set_column_width(sample_table, 1, 90);   // pool
    lv_table_set_column_width(sample_table, 2, 110);  // allocated
    lv_table_set_column_width(sample_table, 3, 110);  // loaded
    lv_table_set_column_width(sample_table, 4, 240);  // format / placement
    lv_obj_set_style_bg_color(sample_table, lv_color_hex(kColCard), LV_PART_ITEMS);
    lv_obj_set_style_text_color(sample_table, lv_color_white(), LV_PART_ITEMS);
    lv_obj_set_style_text_font(sample_table, &lv_font_montserrat_14, LV_PART_ITEMS);
    lv_obj_set_style_border_color(sample_table, lv_color_hex(0x222222), LV_PART_ITEMS);
    lv_obj_set_style_border_width(sample_table, 1, LV_PART_ITEMS);
    lv_obj_set_style_bg_color(sample_table, lv_color_hex(kColCard), LV_PART_MAIN);
    lv_obj_set_style_border_width(sample_table, 0, LV_PART_MAIN);
}

void UIDiagnosticsPage::buildLinkTab(lv_obj_t* tab) {
    static const char* titles[4] = {"LINK", "FRAMES/s", "PACKETS", "ERRORS"};
    // Every figure on this tab is now the frontend's own view of the link.
    // Daisy CPU moved to the Daisy tab; this card kept its "FRAMES/s" title
    // but went on rendering that CPU figure, so the tab showed a percentage
    // under a per-second heading. It now shows what it says: the packet rate
    // derived from the frontend's own counter, which needs neither
    // MSG_DIAG_PUSH nor WAVEX_DAISY_UART_PERF_DEBUG.
    //
    // No sparkline here: pushSpark clamps to 0..100 and the packet rate runs
    // well past that during a preview stream, so the trace would flatline at
    // the top and claim the link had stopped varying.
    static const bool gauge[4] = {false, false, false, false};
    for (int i = 0; i < 4; i++) {
        link_cards[i] =
            makeCard(tab, kColX[i % 2], kRowY[i / 2], kCardW, titles[i], "esp32", gauge[i], 0);
    }

    // Per-message-type counts. wavex_packet_stats_t already tracks these, so
    // the table needs no new plumbing at all.
    lv_obj_t* panel = lv_obj_create(tab);
    lv_obj_remove_style_all(panel);
    lv_obj_add_style(panel, &s_card, LV_PART_MAIN);
    lv_obj_set_size(panel, 622, 464);
    lv_obj_set_pos(panel, kColX[2], 0);
    lv_obj_remove_flag(panel, LV_OBJ_FLAG_SCROLLABLE);

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
    // Slot 5 used to push a standalone Sample Memory page. Its content is the
    // Daisy tab now, so the key would only be a second route to a tab that is
    // already one press of Tab > away.

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
    // One implementation, reading real idle-task runtime. There used to be a
    // second, selectable by WAVEX_CPU_USAGE_METHOD, which *invented* a figure
    // from the number of runnable tasks and how much heap was free. It was
    // unreachable at the configured value, but a diagnostics page that can be
    // switched into reporting a plausible-looking fabricated number is worse
    // than one that reports nothing: the whole purpose of the page is to be
    // believed.
    updateCpuUsageFreertosStats();
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
        case TAB_ESP32:
            refreshEsp32Tab();
            break;
        case TAB_DAISY:
            refreshDaisyTab();
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

void UIDiagnosticsPage::refreshEsp32Tab() {
    if (!esp32_cards[0].value || !lv_obj_is_valid(esp32_cards[0].value)) {
        return;
    }
    char v[48], u[32], sub[64];

    // One tile per core, each with its own trend. The old shared tile showed
    // only the busier core's trace, which is precisely the case where the two
    // differ and the difference matters.
    // No sub line naming what runs on each core: task affinity is set in
    // several places and this page must not assert something it does not read.
    snprintf(v, sizeof(v), "%.1f", cpu_usage_core0);
    setCard(esp32_cards[0], v, "%", "", (int)cpu_usage_core0);
    pushSpark(esp32_cards[0], (int)cpu_usage_core0);

    snprintf(v, sizeof(v), "%.1f", cpu_usage_core1);
    setCard(esp32_cards[1], v, "%", "", (int)cpu_usage_core1);
    pushSpark(esp32_cards[1], (int)cpu_usage_core1);

    const size_t heap_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    const size_t heap_total = heap_caps_get_total_size(MALLOC_CAP_INTERNAL);
    snprintf(v, sizeof(v), "%u", (unsigned)(heap_free / 1024));
    snprintf(sub, sizeof(sub), "of %u KB", (unsigned)(heap_total / 1024));
    setCard(esp32_cards[2],
            v,
            "KB free",
            sub,
            heap_total ? (int)(100 - (heap_free * 100) / heap_total) : 0);

    const size_t ps_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    const size_t ps_total = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
    snprintf(v, sizeof(v), "%.1f", ps_free / (1024.0 * 1024.0));
    snprintf(sub, sizeof(sub), "of %u MB", (unsigned)(ps_total / (1024 * 1024)));
    setCard(
        esp32_cards[3], v, "MB free", sub, ps_total ? (int)(100 - (ps_free * 100) / ps_total) : 0);

    lv_mem_monitor_t mon;
    lv_mem_monitor(&mon);
    snprintf(v, sizeof(v), "%u", (unsigned)((mon.total_size - mon.free_size) / 1024));
    snprintf(u, sizeof(u), "/ %u KB", (unsigned)(mon.total_size / 1024));
    snprintf(sub, sizeof(sub), "fragmentation %u%%", (unsigned)mon.frag_pct);
    setCard(esp32_cards[4], v, u, sub, (int)mon.used_pct);

    // The LVGL task's stack headroom rides on the TASKS card, because an
    // overflow there is a panic, not a slow page - it is what "Stack protection
    // fault in task taskLVGL" was, and the only reason its stack size is now a
    // deliberate 16 KB rather than the port's 7 KB default. High-water mark is
    // the LOW-WATER remaining, so smaller is worse.
    const UBaseType_t lvgl_free = uxTaskGetStackHighWaterMark(nullptr);
    snprintf(v, sizeof(v), "%u", (unsigned)uxTaskGetNumberOfTasks());
    snprintf(sub, sizeof(sub), "LVGL stack free %u B", (unsigned)lvgl_free);
    setCard(esp32_cards[5], v, "running", sub, -1);

    const uint32_t up_s = (uint32_t)(esp_timer_get_time() / 1000000);
    snprintf(v,
             sizeof(v),
             "%luh %02lum",
             (unsigned long)(up_s / 3600),
             (unsigned long)((up_s % 3600) / 60));
    setCard(esp32_cards[6], v, "", "", -1);

    snprintf(v, sizeof(v), "%u", (unsigned)(esp_get_minimum_free_heap_size() / 1024));
    setCard(esp32_cards[7], v, "KB", "lowest since boot", -1);
}

void UIDiagnosticsPage::refreshDaisyTab() {
    if (!daisy_cards[0].value || !lv_obj_is_valid(daisy_cards[0].value)) {
        return;
    }
    char v[48], u[32], sub[80];

    // --- engine CPU and uptime, from the heartbeat (no subscription needed) ---
    wavex_backend_heartbeat_t hb;
    inter_mcu_get_backend_heartbeat_detailed(&hb);
    if (hb.valid) {
        snprintf(v, sizeof(v), "%.1f", hb.cpu_avg_percent);
        snprintf(
            sub, sizeof(sub), "min %.1f%%  max %.1f%%", hb.cpu_min_percent, hb.cpu_max_percent);
        setCard(daisy_cards[0], v, "%", sub, (int)hb.cpu_avg_percent);
        pushSpark(daisy_cards[0], (int)hb.cpu_avg_percent);
    } else {
        setCard(daisy_cards[0], "-", "%", "no heartbeat from backend", 0);
    }

    // Uptime rides on the heartbeat, not on the diagnostics subscription -
    // HeartbeatMessage::uptime_ms has carried it since the link existed, and
    // stage 8 found it there rather than adding a second copy to
    // MSG_DIAG_PUSH. Two cadences reporting one number is two things to
    // disagree.
    //
    // hb.valid gates it for the same reason the placeholder used to say "not
    // on the wire yet": a zero here does not read as "unknown", it reads as a
    // crash loop.
    if (hb.valid) {
        const uint32_t secs = hb.uptime_ms / 1000u;
        snprintf(v,
                 sizeof(v),
                 "%lu:%02lu:%02lu",
                 (unsigned long)(secs / 3600u),
                 (unsigned long)((secs / 60u) % 60u),
                 (unsigned long)(secs % 60u));
        // The heartbeat is the only proof the backend is alive at all, so its
        // own age belongs on the face of the card next to the uptime it
        // carries: a frozen uptime with a stale beat is a hung backend, and a
        // frozen uptime with a fresh beat is impossible.
        const uint32_t beat_now_ms = (uint32_t)(esp_timer_get_time() / 1000);
        snprintf(sub, sizeof(sub), "beat %lu ms ago", (unsigned long)(beat_now_ms - hb.last_rx_ms));
        setCard(daisy_cards[5], v, "", sub, -1);
    } else {
        setCard(daisy_cards[5], "-", "", "no heartbeat from backend", -1);
    }

    // --- system heap, from the diagnostics push ---
    //
    // A different allocator from the sample pools below: newlib's heap in
    // SRAM, where the pools are the SDRAM sample RAM. heap_total == 0 means
    // the backend does not report it - either no push has landed or it is
    // running firmware from before this field existed, since the payload
    // region is zero-padded and an older 94-byte push parses with these two
    // reading zero. A live backend cannot have a zero-byte heap region.
    WaveX::Protocol::DiagPushMessage dp;
    if (inter_mcu_get_diag_push(&dp, kDiagMaxAgeMs) && dp.heap_total != 0) {
        // Used on the face, free in the sub-line and the total in the unit -
        // the same reading order as the two pool cards beside it, so a glance
        // along the row compares like with like.
        const uint32_t used = dp.heap_total - dp.heap_free;
        snprintf(v, sizeof(v), "%lu", (unsigned long)(used / 1024));
        snprintf(u, sizeof(u), "/ %lu KB", (unsigned long)(dp.heap_total / 1024));
        snprintf(sub, sizeof(sub), "%lu KB free", (unsigned long)(dp.heap_free / 1024));
        setCard(daisy_cards[4], v, u, sub, (int)((uint64_t)used * 100 / dp.heap_total));
    } else {
        setCard(daisy_cards[4], "-", "", "no telemetry from backend", 0);
    }

    // --- sample memory ---
    //
    // SampleMemStatus is request/response, not part of the diagnostics
    // subscription, so this tab has to ask for it. The request is issued from
    // here, which runs only while this tab is the active one, rather than from
    // a timer of its own: a per-tab timer is exactly the thing §7 of
    // docs/ui-information-architecture.md warns about, because it outlives the
    // tab that started it. Rate-limited to 1 Hz; the refresh itself runs at
    // 2 Hz behind the sampling timer.
    //
    // inter_mcu_request_sample_mem_status() only enqueues a UART frame, so it
    // does not block the UI task. The reply is stored by the RX task and read
    // back here - store and flag, draw from the timer.
    const uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
    if (sample_req_ms == 0 || (now_ms - sample_req_ms) >= 1000) {
        sample_req_ms = now_ms;
        inter_mcu_request_sample_mem_status();
    }

    wavex_sample_mem_status_t mem;
    inter_mcu_get_sample_mem_status(&mem);
    // A live backend always reports non-zero pool totals, so both being zero
    // means no reply has landed yet rather than "the pools are empty".
    const bool mem_valid = (mem.small_total_bytes != 0 || mem.large_total_bytes != 0);

    if (!mem_valid) {
        for (int i = 1; i <= 3; i++) {
            setCard(daisy_cards[i], "-", "", "no sample memory status yet", -1);
        }
    } else {
        const uint32_t small_used = mem.small_total_bytes - mem.small_free_bytes;
        snprintf(v, sizeof(v), "%lu", (unsigned long)(small_used / 1024));
        snprintf(u, sizeof(u), "/ %lu KB", (unsigned long)(mem.small_total_bytes / 1024));
        snprintf(sub, sizeof(sub), "%lu KB free", (unsigned long)(mem.small_free_bytes / 1024));
        setCard(
            daisy_cards[1], v, u, sub, (int)((uint64_t)small_used * 100 / mem.small_total_bytes));

        const uint32_t large_used = mem.large_total_bytes - mem.large_free_bytes;
        snprintf(v, sizeof(v), "%.1f", large_used / (1024.0 * 1024.0));
        snprintf(u, sizeof(u), "/ %lu MB", (unsigned long)(mem.large_total_bytes / (1024 * 1024)));
        snprintf(sub, sizeof(sub), "%.1f MB free", mem.large_free_bytes / (1024.0 * 1024.0));
        setCard(
            daisy_cards[2],
            v,
            u,
            sub,
            mem.large_total_bytes ? (int)((uint64_t)large_used * 100 / mem.large_total_bytes) : 0);

        // The largest contiguous extent, not the total free: a load fails on
        // this number, not on the sum, and that difference is the whole reason
        // the failed-alloc counter rides alongside it.
        snprintf(v, sizeof(v), "%lu", (unsigned long)(mem.largest_free_bytes / 1024));
        snprintf(sub, sizeof(sub), "%lu failed allocs", (unsigned long)mem.failed_allocs);
        setCard(daisy_cards[3], v, "KB", sub, -1);
    }

    // --- resident samples ---
    if (!sample_table || !lv_obj_is_valid(sample_table)) {
        return;
    }
    // sample_count comes off the wire, so it is clamped rather than trusted:
    // entries[] is fixed at WAVEX_SAMPLE_STATUS_MAX_ENTRIES and a larger count
    // would otherwise size the table for rows there is no data behind.
    const uint8_t n = mem.sample_count < WAVEX_SAMPLE_STATUS_MAX_ENTRIES
                          ? mem.sample_count
                          : (uint8_t)WAVEX_SAMPLE_STATUS_MAX_ENTRIES;
    lv_table_set_row_count(sample_table, n ? (uint32_t)n + 1 : 2);
    lv_table_set_cell_value(sample_table, 0, 0, "ID");
    lv_table_set_cell_value(sample_table, 0, 1, "POOL");
    lv_table_set_cell_value(sample_table, 0, 2, "ALLOC");
    lv_table_set_cell_value(sample_table, 0, 3, "LOADED");
    lv_table_set_cell_value(sample_table, 0, 4, "FORMAT / PLACEMENT");
    for (uint8_t i = 0; i < n; i++) {
        const auto& e = mem.entries[i];
        char cell[64];
        snprintf(cell, sizeof(cell), "%u", (unsigned)e.sample_id);
        lv_table_set_cell_value(sample_table, i + 1, 0, cell);
        // cls 0xFF is the large-pool sentinel; anything else is a small-pool
        // size class.
        lv_table_set_cell_value(sample_table, i + 1, 1, e.cls == 0xFF ? "Large" : "Small");
        snprintf(cell, sizeof(cell), "%lu KB", (unsigned long)(e.allocated_bytes / 1024));
        lv_table_set_cell_value(sample_table, i + 1, 2, cell);
        snprintf(cell, sizeof(cell), "%lu KB", (unsigned long)(e.loaded_bytes / 1024));
        lv_table_set_cell_value(sample_table, i + 1, 3, cell);
        snprintf(cell,
                 sizeof(cell),
                 "%lu Hz %uch %ub  cls=%u p=%u s=%u",
                 (unsigned long)e.sample_rate,
                 (unsigned)e.channels,
                 (unsigned)e.bit_depth,
                 (unsigned)e.cls,
                 (unsigned)e.page,
                 (unsigned)e.slot);
        lv_table_set_cell_value(sample_table, i + 1, 4, cell);
    }
    if (n == 0) {
        lv_table_set_cell_value(sample_table, 1, 0, "-");
        lv_table_set_cell_value(sample_table, 1, 1, "");
        lv_table_set_cell_value(sample_table, 1, 2, "");
        lv_table_set_cell_value(sample_table, 1, 3, "");
        lv_table_set_cell_value(
            sample_table, 1, 4, mem_valid ? "none resident" : "awaiting backend");
    }
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

    // Frames/s over a >=1 s window of the frontend's own packet counter. The
    // card is titled FRAMES/s and used to render the Daisy's CPU percentage,
    // which now has a tab of its own; a rate needs a window, so the reference
    // point is held between refreshes rather than recomputed from nothing.
    if (frames_ref_ms == 0) {
        frames_ref_ms = uptime_ms;
        frames_ref_pkts = st.total_packets;
    } else if (uptime_ms - frames_ref_ms >= 1000) {
        const uint32_t d_ms = uptime_ms - frames_ref_ms;
        // Counters are monotonic, but a reset would wrap this; clamp rather
        // than print a nonsense spike.
        const uint32_t d_pkts =
            st.total_packets >= frames_ref_pkts ? st.total_packets - frames_ref_pkts : 0;
        frames_per_s = (uint32_t)(((uint64_t)d_pkts * 1000ull) / d_ms);
        frames_ref_ms = uptime_ms;
        frames_ref_pkts = st.total_packets;
    }
    snprintf(v, sizeof(v), "%lu", (unsigned long)frames_per_s);
    setCard(link_cards[1], v, "/s", "packets, 1 s window", -1);

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
        // Not a link statistic, but this table is where someone looks when
        // input feels lost, and a non-zero value here is the direct answer.
        {"INPUT DROPPED", InputDispatcher::instance().droppedEvents()},
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
