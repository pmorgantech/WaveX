/**
 * Off-host UI preview: renders WaveX screens with the SAME LVGL the firmware
 * vendors (managed_components/lvgl__lvgl), the same fonts, palette and
 * chrome geometry (ui_theme.h values duplicated below), into BMP files.
 *
 * No hardware, no serial dump: LVGL's software renderer draws into a plain
 * memory buffer at the logical resolution (1280x720 landscape - the device
 * rotates in software after rendering, so this is exactly what the renderer
 * produces there too).
 *
 * Screens rendered:
 *   01_diag_current.bmp     replica of today's diagnostics page
 *   02_diag_proposed.bmp    proposed tabbed diagnostics (System tab active)
 *   03_browser_proposed.bmp proposed sample browser (list + detail pane)
 *
 * These are previews for design iteration, not captures of the firmware
 * pages themselves: the real pages depend on ESP-IDF services and are not
 * compiled here (yet - see docs/backlog.md).
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lvgl.h"

/* ---- device geometry & palette: keep in sync with ui_theme.h ---- */
#define HOR 1280
#define VER 720
#define HEADER_H 75   /* UI_HEADER_HEIGHT */
#define SOFTKEY_H 100 /* UI_HOTKEY_HEIGHT */
#define N_SOFTKEYS 6

#define C_BG lv_color_make(0x00, 0x00, 0x00)
#define C_HEADER lv_color_make(0x2E, 0x34, 0x40)
#define C_PANEL lv_color_make(0x1A, 0x1A, 0x1A)
#define C_BORDER lv_color_make(0x33, 0x33, 0x33)
#define C_TEXT lv_color_make(0xFF, 0xFF, 0xFF)
#define C_BLUE lv_color_make(0x21, 0x96, 0xF3)
#define C_GREEN lv_color_make(0x4C, 0xAF, 0x50)
#define C_ORANGE lv_color_make(0xFF, 0x57, 0x22)

static uint16_t fb[HOR * VER];

static uint32_t s_tick;
static uint32_t tick_cb(void) {
    return s_tick;
}

static void flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map) {
    LV_UNUSED(area);
    LV_UNUSED(px_map);
    lv_display_flush_ready(disp); /* DIRECT mode: fb already holds the pixels */
}

/* Minimal 24-bit BMP writer (stdlib only, bottom-up rows). */
static void write_bmp(const char *path) {
    const uint32_t row = HOR * 3;
    const uint32_t pad = (4 - (row % 4)) % 4;
    const uint32_t data = (row + pad) * VER;
    const uint32_t off = 54;
    uint8_t hdr[54] = {0};
    hdr[0] = 'B';
    hdr[1] = 'M';
    uint32_t fsz = off + data;
    memcpy(hdr + 2, &fsz, 4);
    memcpy(hdr + 10, &off, 4);
    uint32_t bisz = 40;
    memcpy(hdr + 14, &bisz, 4);
    int32_t w = HOR, h = VER;
    memcpy(hdr + 18, &w, 4);
    memcpy(hdr + 22, &h, 4);
    uint16_t planes = 1, bpp = 24;
    memcpy(hdr + 26, &planes, 2);
    memcpy(hdr + 28, &bpp, 2);
    memcpy(hdr + 34, &data, 4);

    FILE *f = fopen(path, "wb");
    if (!f) {
        perror(path);
        exit(1);
    }
    fwrite(hdr, 1, 54, f);
    uint8_t padb[3] = {0};
    for (int y = VER - 1; y >= 0; y--) {
        for (int x = 0; x < HOR; x++) {
            uint16_t p = fb[y * HOR + x];
            uint8_t bgr[3] = {
                (uint8_t)((p & 0x1F) << 3),        /* B */
                (uint8_t)(((p >> 5) & 0x3F) << 2), /* G */
                (uint8_t)(((p >> 11) & 0x1F) << 3) /* R */
            };
            fwrite(bgr, 1, 3, f);
        }
        if (pad)
            fwrite(padb, 1, pad, f);
    }
    fclose(f);
    printf("wrote %s\n", path);
}

/* ---- chrome: header + content area + 6-softkey bar, per ui_theme.h ---- */
static lv_obj_t *make_chrome(const char *title, const char *keys[N_SOFTKEYS]) {
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, C_BG, 0);
    lv_obj_set_style_pad_all(scr, 0, 0);

    lv_obj_t *hdr = lv_obj_create(scr);
    lv_obj_set_size(hdr, HOR, HEADER_H);
    lv_obj_align(hdr, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_style_bg_color(hdr, C_HEADER, 0);
    lv_obj_set_style_border_width(hdr, 0, 0);
    lv_obj_set_style_radius(hdr, 0, 0);
    lv_obj_t *t = lv_label_create(hdr);
    lv_label_set_text(t, title);
    lv_obj_set_style_text_font(t, &lv_font_montserrat_36, 0);
    lv_obj_set_style_text_color(t, C_TEXT, 0);
    lv_obj_center(t);

    lv_obj_t *bar = lv_obj_create(scr);
    lv_obj_set_size(bar, HOR, SOFTKEY_H);
    lv_obj_align(bar, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    lv_obj_set_style_bg_color(bar, C_BG, 0);
    lv_obj_set_style_border_width(bar, 0, 0);
    lv_obj_set_style_radius(bar, 0, 0);
    lv_obj_set_style_pad_all(bar, 6, 0);
    int bw = (HOR - 12) / N_SOFTKEYS;
    for (int i = 0; i < N_SOFTKEYS; i++) {
        lv_obj_t *b = lv_button_create(bar);
        lv_obj_set_size(b, bw - 8, SOFTKEY_H - 20);
        lv_obj_set_pos(b, i * bw + 4, 0);
        lv_obj_set_style_bg_color(b, keys[i] ? C_BLUE : C_PANEL, 0);
        lv_obj_set_style_radius(b, 5, 0); /* UI_BORDER_RADIUS */
        if (keys[i]) {
            lv_obj_t *l = lv_label_create(b);
            lv_label_set_text(l, keys[i]);
            lv_obj_set_style_text_font(l, &lv_font_montserrat_36, 0);
            lv_obj_set_style_text_color(l, C_TEXT, 0);
            lv_obj_center(l);
        }
    }

    lv_obj_t *content = lv_obj_create(scr);
    lv_obj_set_size(content, HOR, VER - HEADER_H - SOFTKEY_H);
    lv_obj_align(content, LV_ALIGN_TOP_LEFT, 0, HEADER_H);
    lv_obj_set_style_bg_color(content, C_BG, 0);
    lv_obj_set_style_border_width(content, 0, 0);
    lv_obj_set_style_radius(content, 0, 0);
    lv_obj_set_style_pad_all(content, 8, 0);
    lv_obj_set_user_data(scr, content);
    return scr;
}

static void render(lv_display_t *disp, lv_obj_t *scr, const char *path) {
    lv_screen_load(scr);
    s_tick += 50;
    lv_timer_handler();
    lv_refr_now(disp);
    write_bmp(path);
}

static lv_obj_t *panel(lv_obj_t *parent) {
    lv_obj_t *p = lv_obj_create(parent);
    lv_obj_set_style_bg_color(p, C_PANEL, 0);
    lv_obj_set_style_border_width(p, 1, 0);
    lv_obj_set_style_border_color(p, C_BORDER, 0);
    lv_obj_set_style_radius(p, 5, 0);
    lv_obj_set_style_pad_all(p, 10, 0);
    return p;
}

static lv_obj_t *text(lv_obj_t *parent, const char *s, const lv_font_t *f, lv_color_t c) {
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, s);
    lv_obj_set_style_text_font(l, f, 0);
    lv_obj_set_style_text_color(l, c, 0);
    return l;
}

/* ---- 01: replica of the current diagnostics page ---- */
static void diag_current(lv_display_t *disp) {
    const char *keys[N_SOFTKEYS] = {"Back", "Samples", NULL, NULL, NULL, NULL};
    lv_obj_t *scr = make_chrome("Diagnostics", keys);
    lv_obj_t *content = lv_obj_get_user_data(scr);
    lv_obj_set_flex_flow(content, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(
        content, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

    const char *cols[3] = {"ESP32 Status", "Daisy Link", "Meters"};
    const char *bodies[3] = {
        "CPU: 100.0%\nCore0: 100.0%\nCore1: 100.0%\nHeap: 27242 KB\nTasks: 14",
        "Link: OK\nHeartbeat: 12s ago\nRX: 4821\nLoop: 481885\nCPU: 4.2%",
        "RMS L: -32 dB\nRMS R: -33 dB\nPeak L: -12 dB\nPeak R: -13 dB",
    };
    for (int i = 0; i < 3; i++) {
        lv_obj_t *col = panel(content);
        lv_obj_set_size(col, lv_pct(30), lv_pct(100));
        lv_obj_t *title = text(col, cols[i], &lv_font_montserrat_26, C_TEXT);
        lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 5);
        lv_obj_t *body = text(col, bodies[i], &lv_font_montserrat_22, C_TEXT);
        lv_obj_align(body, LV_ALIGN_TOP_LEFT, 0, 45);
    }
    render(disp, scr, "out/01_diag_current.bmp");
}

/* ---- helpers for the proposed screens ---- */
static void stat_tile(lv_obj_t *grid,
                      int col,
                      int row,
                      const char *name,
                      const char *val,
                      int pct,
                      lv_color_t bar_color) {
    lv_obj_t *tile = panel(grid);
    lv_obj_set_grid_cell(tile, LV_GRID_ALIGN_STRETCH, col, 1, LV_GRID_ALIGN_STRETCH, row, 1);
    lv_obj_t *n = text(tile, name, &lv_font_montserrat_18, lv_color_make(0xAA, 0xAA, 0xAA));
    lv_obj_align(n, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_t *v = text(tile, val, &lv_font_montserrat_28, C_TEXT);
    lv_obj_align(v, LV_ALIGN_LEFT_MID, 0, 4);
    if (pct >= 0) {
        lv_obj_t *bar = lv_bar_create(tile);
        lv_obj_set_size(bar, lv_pct(100), 12);
        lv_obj_align(bar, LV_ALIGN_BOTTOM_LEFT, 0, 0);
        lv_bar_set_value(bar, pct, LV_ANIM_OFF);
        lv_obj_set_style_bg_color(bar, C_BORDER, LV_PART_MAIN);
        lv_obj_set_style_bg_color(bar, bar_color, LV_PART_INDICATOR);
        lv_obj_set_style_radius(bar, 3, LV_PART_MAIN);
        lv_obj_set_style_radius(bar, 3, LV_PART_INDICATOR);
    }
}

/* ---- 02: proposed tabbed diagnostics ---- */
static void diag_proposed(lv_display_t *disp) {
    const char *keys[N_SOFTKEYS] = {"Back", "Tab <", "Tab >", "Freeze", NULL, "Reset"};
    lv_obj_t *scr = make_chrome("Diagnostics", keys);
    lv_obj_t *content = lv_obj_get_user_data(scr);
    lv_obj_set_style_pad_all(content, 0, 0);

    lv_obj_t *tv = lv_tabview_create(content);
    lv_tabview_set_tab_bar_size(tv, 52);
    lv_obj_set_size(tv, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(tv, C_BG, 0);
    lv_obj_t *bar = lv_tabview_get_tab_bar(tv);
    lv_obj_set_style_bg_color(bar, C_HEADER, 0);
    lv_obj_set_style_text_font(bar, &lv_font_montserrat_22, 0);

    lv_obj_t *t_sys = lv_tabview_add_tab(tv, "System");
    lv_tabview_add_tab(tv, "Audio");
    lv_tabview_add_tab(tv, "Link");
    lv_tabview_add_tab(tv, "Storage");
    lv_tabview_add_tab(tv, "MIDI");

    lv_obj_set_style_bg_color(t_sys, C_BG, 0);
    static int32_t cols[] = {
        LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_TEMPLATE_LAST};
    static int32_t rows[] = {LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_TEMPLATE_LAST};
    lv_obj_set_grid_dsc_array(t_sys, cols, rows);
    lv_obj_set_style_pad_all(t_sys, 8, 0);
    lv_obj_set_style_pad_gap(t_sys, 8, 0);

    stat_tile(t_sys, 0, 0, "CPU CORE 0", "7.4%", 7, C_GREEN);
    stat_tile(t_sys, 1, 0, "CPU CORE 1", "3.1%", 3, C_GREEN);
    stat_tile(t_sys, 2, 0, "HEAP INTERNAL", "412 KB free", 62, C_GREEN);
    stat_tile(t_sys, 3, 0, "PSRAM", "26.0 MB free", 81, C_GREEN);
    stat_tile(t_sys, 0, 1, "LVGL POOL", "74 KB / 128 KB", 58, C_ORANGE);
    stat_tile(t_sys, 1, 1, "UI FRAME TIME", "11 ms", 34, C_GREEN);
    stat_tile(t_sys, 2, 1, "UPTIME", "2h 41m", -1, C_GREEN);
    stat_tile(t_sys, 3, 1, "MIN FREE HEAP", "233 KB", -1, C_GREEN);

    render(disp, scr, "out/02_diag_proposed.bmp");
}

/* ---- 03: proposed sample browser ---- */
static void browser_proposed(lv_display_t *disp) {
    const char *keys[N_SOFTKEYS] = {"Back", "Audition", "Load", "Edit", "Sort", "Eject"};
    lv_obj_t *scr = make_chrome("Samples  /drums", keys);
    lv_obj_t *content = lv_obj_get_user_data(scr);

    /* Left: file list, 58% */
    lv_obj_t *list = panel(content);
    lv_obj_set_size(list, lv_pct(58), lv_pct(100));
    lv_obj_align(list, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_style_pad_all(list, 4, 0);
    const char *names[] = {"..",
                           "808-kick.wav",
                           "amen-full.wav",
                           "clap-tight.wav",
                           "hat-open.wav",
                           "ride-bell.wav",
                           "snare-909.wav"};
    const char *durs[] = {"", "0:01", "0:07", "0:01", "0:02", "0:03", "0:01"};
    for (int i = 0; i < 7; i++) {
        lv_obj_t *row = lv_obj_create(list);
        lv_obj_set_size(row, lv_pct(100), 56);
        lv_obj_set_pos(row, 0, i * 60);
        lv_obj_set_style_bg_color(row, i == 2 ? lv_color_make(0x10, 0x2A, 0x14) : C_PANEL, 0);
        lv_obj_set_style_border_width(row, i == 2 ? 2 : 0, 0);
        lv_obj_set_style_border_color(row, C_GREEN, 0);
        lv_obj_set_style_radius(row, 5, 0);
        lv_obj_set_style_pad_all(row, 8, 0);
        lv_obj_t *n = text(row, names[i], &lv_font_montserrat_22, C_TEXT);
        lv_obj_align(n, LV_ALIGN_LEFT_MID, 0, 0);
        if (durs[i][0]) {
            lv_obj_t *d =
                text(row, durs[i], &lv_font_montserrat_18, lv_color_make(0xAA, 0xAA, 0xAA));
            lv_obj_align(d, LV_ALIGN_RIGHT_MID, 0, 0);
        }
    }

    /* Right: detail pane, 40% */
    lv_obj_t *det = panel(content);
    lv_obj_set_size(det, lv_pct(40), lv_pct(100));
    lv_obj_align(det, LV_ALIGN_TOP_RIGHT, 0, 0);
    lv_obj_t *fn = text(det, "amen-full.wav", &lv_font_montserrat_26, C_TEXT);
    lv_obj_align(fn, LV_ALIGN_TOP_LEFT, 0, 0);

    /* waveform: chart standing in for the existing waveform canvas */
    lv_obj_t *ch = lv_chart_create(det);
    lv_obj_set_size(ch, lv_pct(100), 200);
    lv_obj_align(ch, LV_ALIGN_TOP_LEFT, 0, 44);
    lv_chart_set_type(ch, LV_CHART_TYPE_LINE);
    lv_chart_set_point_count(ch, 64);
    lv_chart_set_range(ch, LV_CHART_AXIS_PRIMARY_Y, -100, 100);
    lv_obj_set_style_bg_color(ch, lv_color_make(0x0A, 0x0A, 0x0A), 0);
    lv_obj_set_style_line_width(ch, 2, LV_PART_ITEMS);
    lv_chart_series_t *ser = lv_chart_add_series(ch, C_GREEN, LV_CHART_AXIS_PRIMARY_Y);
    for (int i = 0; i < 64; i++) {
        int v = (i * 37 % 173) % 200 - 100;
        lv_chart_set_next_value(ch, ser, v);
    }

    lv_obj_t *meta = text(det,
                          "44100 Hz   16-bit   stereo\n0:07.42   1.3 MB\ndata @ 44  (aligned)",
                          &lv_font_montserrat_18,
                          lv_color_make(0xAA, 0xAA, 0xAA));
    lv_obj_align(meta, LV_ALIGN_TOP_LEFT, 0, 258);

    lv_obj_t *state = text(det, LV_SYMBOL_PLAY "  AUDITIONING", &lv_font_montserrat_22, C_GREEN);
    lv_obj_align(state, LV_ALIGN_BOTTOM_LEFT, 0, 0);

    render(disp, scr, "out/03_browser_proposed.bmp");
}

int main(void) {
    lv_init();
    lv_tick_set_cb(tick_cb);

    lv_display_t *disp = lv_display_create(HOR, VER);
    lv_display_set_buffers(disp, fb, NULL, sizeof(fb), LV_DISPLAY_RENDER_MODE_DIRECT);
    lv_display_set_flush_cb(disp, flush_cb);

    diag_current(disp);
    diag_proposed(disp);
    browser_proposed(disp);
    return 0;
}
