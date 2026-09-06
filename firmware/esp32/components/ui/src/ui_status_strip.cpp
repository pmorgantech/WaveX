// WaveX header status strip: output meters + engine CPU
#include "ui/ui_status_strip.h"

#include "../styles/ui_theme.h"
#include "ui/ui_palette.h"

#include <cstdio>

#ifdef ESP_PLATFORM
#include "esp_timer.h"
// inter_mcu is C++ with no extern "C" guard - include it plainly, as
// ui_diagnostics_page.cpp does, or the names will not resolve.
#include "inter_mcu.h"
#endif

namespace wavex_ui {
namespace {

// Geometry, right-anchored inside the header. Reading right to left the
// header ends with the SHIFT chip, then the engine-CPU block, then the eight
// meter columns - each separated by UI_HEADER_GAP. Everything is derived from
// UI_SHIFT_CHIP_X rather than from the screen edge, so moving the chip moves
// the strip with it instead of letting the two overlap.
constexpr int kMeters = 8;
constexpr int kMeterW = 9;
constexpr int kMeterGap = 3;
constexpr int kMeterPitch = kMeterW + kMeterGap;
constexpr int kMeterH = 40;
constexpr int kMeterY = (UI_HEADER_HEIGHT - kMeterH) / 2;
constexpr int kMetersW = kMeters * kMeterW + (kMeters - 1) * kMeterGap;

// The CPU block is a right-aligned label over a slim bar, not a bar with a
// label beside it: the number is what gets read, the bar is peripheral.
constexpr int kCpuBlockW = 90;
constexpr int kCpuBlockX = UI_SHIFT_CHIP_X - UI_HEADER_GAP - kCpuBlockW;
constexpr int kCpuBarW = kCpuBlockW;
constexpr int kCpuBarH = 6;
constexpr int kCpuLabelY = 16;
constexpr int kCpuBarY = 42;

constexpr int kMeterX0 = kCpuBlockX - UI_HEADER_GAP - kMetersW;

// An idle column is a 3px base rather than a full-height empty track: the
// track shape implies a level is being measured, and for six of these eight
// nothing is on the wire yet.
constexpr int kStubH = 3;

// Colours. The strip is chrome, so it uses its own flat palette rather than
// the page theme - it must read the same on every screen behind it.
// Local names for the shared palette (ui/ui_palette.h). These were
// hand-copied literals that had already drifted from it and from each
// other - three different "border" greys existed across five files - so a
// theme switch reached only the surfaces that happened to be in sync.
constexpr uint32_t kColMeterBg = palette::kColCardAlt;
constexpr uint32_t kColGreen = palette::kColGreen;
constexpr uint32_t kColOrange = palette::kColOrange;
constexpr uint32_t kColStub = palette::kColBorder;
constexpr uint32_t kColDim = palette::kColDim;

// A meter push older than this is treated as silence. Without it the last
// levels before a link drop stay lit forever, which reads as "still playing".
constexpr uint32_t kMeterStaleMs = 500;

// Peak hold: a raw peak sampled every 100 ms tracks the RMS closely enough to
// be redundant, and flickers. Holding then decaying is what makes the tick
// readable - and what makes a brief clip visible at all.
constexpr uint32_t kPeakHoldMs = 800;
constexpr float kPeakDecayPerTick = 0.03f;  // ~0.3/s at a 100 ms tick

struct Column {
    lv_obj_t* track;  // background, active columns only
    lv_obj_t* fill;   // RMS
    lv_obj_t* peak;   // peak tick
    int last_h;       // last applied heights, so an idle strip stops
    int last_p;       // invalidating the header ten times a second
    float peak_held;
    uint32_t peak_at_ms;
};

Column s_col[kMeters];
lv_obj_t* s_cpu_label = nullptr;
lv_obj_t* s_cpu_bar = nullptr;
lv_timer_t* s_timer = nullptr;
int s_last_cpu_pct = -2;  // neither a valid percent nor the "no link" -1

lv_obj_t* box(lv_obj_t* parent, int x, int y, int w, int h, uint32_t colour) {
    lv_obj_t* o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_size(o, w, h);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_style_bg_color(o, lv_color_hex(colour), 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    return o;
}

float clamp01(float v) {
    return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
}

// Sets one column from a 0..1 RMS and peak pair, growing upward from the base.
void setColumn(Column& c, float rms, float peak, uint32_t now_ms) {
    if (!c.fill) {
        return;
    }
    if (peak >= c.peak_held) {
        c.peak_held = clamp01(peak);
        c.peak_at_ms = now_ms;
    } else if (now_ms - c.peak_at_ms > kPeakHoldMs) {
        c.peak_held -= kPeakDecayPerTick;
        if (c.peak_held < 0.0f) {
            c.peak_held = 0.0f;
        }
    }

    const int h = static_cast<int>(kMeterH * clamp01(rms) + 0.5f);
    if (h != c.last_h) {
        c.last_h = h;
        if (h > 0) {
            lv_obj_set_size(c.fill, kMeterW, h);
            lv_obj_set_y(c.fill, kMeterY + kMeterH - h);
            lv_obj_remove_flag(c.fill, LV_OBJ_FLAG_HIDDEN);
            // Near full scale is where clipping starts to matter, so the fill
            // changes colour rather than just getting taller.
            lv_obj_set_style_bg_color(
                c.fill, lv_color_hex(rms >= 0.9f ? kColOrange : kColGreen), 0);
        } else {
            lv_obj_add_flag(c.fill, LV_OBJ_FLAG_HIDDEN);
        }
    }

    const int p = static_cast<int>(kMeterH * c.peak_held + 0.5f);
    if (p != c.last_p) {
        c.last_p = p;
        if (p > 1) {
            lv_obj_set_y(c.peak, kMeterY + kMeterH - p);
            lv_obj_remove_flag(c.peak, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(c.peak, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

void tick(lv_timer_t*) {
#ifdef ESP_PLATFORM
    wavex_meter_data_t md;
    inter_mcu_get_meter_data(&md);

    const uint32_t now_ms = static_cast<uint32_t>(esp_timer_get_time() / 1000);
    const bool fresh =
        md.valid && md.last_update_ms > 0 && (now_ms - md.last_update_ms) < kMeterStaleMs;

    setColumn(s_col[0], fresh ? md.rms_left : 0.0f, fresh ? md.peak_left : 0.0f, now_ms);
    setColumn(s_col[1], fresh ? md.rms_right : 0.0f, fresh ? md.peak_right : 0.0f, now_ms);

    // Engine CPU, not UI CPU: this is the number that predicts an underrun.
    wavex_backend_heartbeat_t hb;
    inter_mcu_get_backend_heartbeat_detailed(&hb);
    char buf[16];
    int pct = 0;
    // Liveness, not "have we ever heard from it" - hb.valid latches on the
    // first heartbeat, so this readout used to hold the last CPU figure it saw
    // forever after a link drop. The meters beside it already decay on
    // staleness; this now matches them.
    if (hb.valid && inter_mcu_backend_link_alive()) {
        pct = static_cast<int>(hb.cpu_avg_percent + 0.5f);
        if (pct < 0)
            pct = 0;
        if (pct > 100)
            pct = 100;
        snprintf(buf, sizeof(buf), "CPU %d%%", pct);
    } else {
        snprintf(buf, sizeof(buf), "CPU --");
        pct = -1;  // distinct from a genuine 0%, so the bar is not redrawn
    }
    if (pct != s_last_cpu_pct) {
        s_last_cpu_pct = pct;
        lv_label_set_text(s_cpu_label, buf);
        const int bar_pct = pct < 0 ? 0 : pct;
        lv_obj_set_width(s_cpu_bar, (kCpuBarW * bar_pct) / 100);
        lv_obj_set_style_bg_color(
            s_cpu_bar, lv_color_hex(bar_pct >= 85 ? kColOrange : kColGreen), 0);
    }
#endif
}

}  // namespace

void statusStripCreate(lv_obj_t* header) {
    if (!header) {
        return;
    }
    if (s_timer) {
        lv_timer_delete(s_timer);
        s_timer = nullptr;
    }

    // Absolute positioning inside the header needs its padding out of the way;
    // the title is centred, so this does not move it.
    lv_obj_set_style_pad_all(header, 0, 0);
    lv_obj_remove_flag(header, LV_OBJ_FLAG_SCROLLABLE);
    s_last_cpu_pct = -2;

    for (int i = 0; i < kMeters; i++) {
        const int x = kMeterX0 + i * kMeterPitch;
        s_col[i] = Column{};
        if (i < 2) {
            s_col[i].track = box(header, x, kMeterY, kMeterW, kMeterH, kColMeterBg);
            s_col[i].fill = box(header, x, kMeterY + kMeterH, kMeterW, 0, kColGreen);
            s_col[i].peak = box(header, x, kMeterY + kMeterH, kMeterW, 2, palette::kColFg);
            lv_obj_add_flag(s_col[i].fill, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(s_col[i].peak, LV_OBJ_FLAG_HIDDEN);
        } else {
            // No per-voice meters on the wire yet: a 2px base stub says the
            // channel exists and is idle, which is true, where a full-height
            // empty track would imply we are measuring it.
            box(header, x, kMeterY + kMeterH - kStubH, kMeterW, kStubH, kColStub);
        }
    }

    s_cpu_label = lv_label_create(header);
    // Mono: the percentage is re-rendered ten times a second, and a
    // proportional face makes the label's width jitter as the digits change.
    lv_obj_set_style_text_font(s_cpu_label, UI_FONT_MONO_MICRO, 0);
    lv_obj_set_style_text_color(s_cpu_label, lv_color_hex(kColDim), 0);
    lv_label_set_text(s_cpu_label, "CPU --");
    // Right-aligned within its block so the label grows leftward, towards the
    // meters, and never pushes into the SHIFT chip.
    lv_obj_set_pos(s_cpu_label, kCpuBlockX, kCpuLabelY);
    lv_obj_set_width(s_cpu_label, kCpuBlockW);
    lv_obj_set_style_text_align(s_cpu_label, LV_TEXT_ALIGN_RIGHT, 0);

    lv_obj_t* cpu_track = box(header, kCpuBlockX, kCpuBarY, kCpuBarW, kCpuBarH, kColMeterBg);
    lv_obj_set_style_radius(cpu_track, kCpuBarH / 2, 0);
    s_cpu_bar = box(header, kCpuBlockX, kCpuBarY, 0, kCpuBarH, kColGreen);
    lv_obj_set_style_radius(s_cpu_bar, kCpuBarH / 2, 0);

    s_timer = lv_timer_create(tick, 100, nullptr);
}

}  // namespace wavex_ui
