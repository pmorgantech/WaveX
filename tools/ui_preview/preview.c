/**
 * Off-host UI preview: renders WaveX screens with the SAME LVGL the firmware
 * vendors (managed_components/lvgl__lvgl), the same fonts, palette and chrome
 * geometry, into BMP files.
 *
 * No hardware, no serial dump: LVGL's software renderer draws into a plain
 * memory buffer at the logical resolution (1280x720 landscape - the device
 * rotates in software after rendering, so this is exactly what the renderer
 * produces there too).
 *
 * Screens 01-07 implement "WaveX Wireframes v2.dc.html" from the Claude
 * Design project, using that design's own coordinates, colours and sample
 * content so a render can be compared against the wireframe directly:
 *
 *   01_diag_system.bmp    Diagnostics / System   (design 1a)
 *   02_diag_audio.bmp     Diagnostics / Audio    (design 2a)
 *   03_diag_link.bmp      Diagnostics / Link     (design 2b)
 *   04_diag_storage.bmp   Diagnostics / Storage  (design 2c)
 *   05_diag_midi.bmp      Diagnostics / MIDI     (design 2d)
 *   06_sample_browser.bmp Sample browser         (design 1b)
 *   07_sample_edit.bmp    Sample edit            (design 2e)
 *
 * These are previews for design iteration, not captures of the firmware
 * pages: the real pages depend on ESP-IDF services and are not compiled here.
 * The widget choices and geometry are the ones the firmware should use, so
 * this doubles as the reference for the port.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lvgl.h"

/* ---- device geometry: keep in sync with ui_theme.h ---- */
#define HOR 1280
#define VER 720
#define HEADER_H 75   /* UI_HEADER_HEIGHT */
#define SOFTKEY_H 100 /* UI_HOTKEY_HEIGHT */
#define N_SOFTKEYS 6

/* Design palette (WaveX Wireframes v2) - superset of ui_theme.h */
#define C_BG lv_color_hex(0x000000)
#define C_HEADER lv_color_hex(0x2E3440)
#define C_CARD lv_color_hex(0x141414)
#define C_BORDER lv_color_hex(0x333333)
#define C_TEXT lv_color_hex(0xFFFFFF)
#define C_DIM lv_color_hex(0x8FA0AA)
#define C_DIMMER lv_color_hex(0x6E7A82)
#define C_BLUE lv_color_hex(0x2196F3)
#define C_GREEN lv_color_hex(0x4CAF50)
#define C_ORANGE lv_color_hex(0xFF5722)
#define C_TRACK lv_color_hex(0x262B2E)
#define C_TABON lv_color_hex(0x10293B)
#define C_SELBG lv_color_hex(0x0F2A16)
#define C_LISTBG lv_color_hex(0x0D0D0D)
#define C_PANEL lv_color_hex(0x121212)
#define C_STUB lv_color_hex(0x3A4148)
#define C_METERBG lv_color_hex(0x20262B)

/* Card anatomy (design 1a) */
#define CARD_W 305
#define CARD_H 226
#define GAUGE_W 273

static uint16_t fb[HOR * VER];
static uint32_t s_tick;
static uint32_t tick_cb(void) {
    return s_tick;
}

static void flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map) {
    LV_UNUSED(area);
    LV_UNUSED(px_map);
    lv_display_flush_ready(disp);
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
            uint8_t bgr[3] = {(uint8_t)((p & 0x1F) << 3),
                              (uint8_t)(((p >> 5) & 0x3F) << 2),
                              (uint8_t)(((p >> 11) & 0x1F) << 3)};
            fwrite(bgr, 1, 3, f);
        }
        if (pad)
            fwrite(padb, 1, pad, f);
    }
    fclose(f);
    printf("wrote %s\n", path);
}

/* ---- small helpers ---- */

static lv_obj_t *plain(lv_obj_t *parent, int x, int y, int w, int h, lv_color_t bg) {
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_set_size(o, w, h);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_style_bg_color(o, bg, 0);
    lv_obj_set_style_border_width(o, 0, 0);
    lv_obj_set_style_radius(o, 0, 0);
    lv_obj_set_style_pad_all(o, 0, 0);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    return o;
}

static lv_obj_t *text_at(
    lv_obj_t *p, int x, int y, const char *s, const lv_font_t *f, lv_color_t c) {
    lv_obj_t *l = lv_label_create(p);
    lv_label_set_text(l, s);
    lv_obj_set_style_text_font(l, f, 0);
    lv_obj_set_style_text_color(l, c, 0);
    lv_obj_set_pos(l, x, y);
    return l;
}

/* lv_bar with the design's track/fill treatment. */
static lv_obj_t *bar_at(lv_obj_t *p, int x, int y, int w, int h, int pct, lv_color_t fill) {
    lv_obj_t *b = lv_bar_create(p);
    lv_obj_set_size(b, w, h);
    lv_obj_set_pos(b, x, y);
    lv_bar_set_range(b, 0, 100);
    lv_bar_set_value(b, pct, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(b, C_TRACK, LV_PART_MAIN);
    lv_obj_set_style_bg_color(b, fill, LV_PART_INDICATOR);
    lv_obj_set_style_radius(b, 2, LV_PART_MAIN);
    lv_obj_set_style_radius(b, 2, LV_PART_INDICATOR);
    return b;
}

/* 60-point rolling sparkline: lv_chart, line only, no points (design 2a). */
static void spark_at(
    lv_obj_t *p, int x, int y, int w, int h, lv_color_t c, int base, int amp, int slope) {
    lv_obj_t *ch = lv_chart_create(p);
    lv_obj_set_size(ch, w, h);
    lv_obj_set_pos(ch, x, y);
    lv_chart_set_type(ch, LV_CHART_TYPE_LINE);
    lv_chart_set_point_count(ch, 60);
    lv_chart_set_div_line_count(ch, 0, 0);
    lv_chart_set_range(ch, LV_CHART_AXIS_PRIMARY_Y, 0, 100);
    lv_obj_set_style_bg_opa(ch, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(ch, 0, 0);
    lv_obj_set_style_pad_all(ch, 0, 0);
    lv_obj_set_style_size(ch, 0, 0, LV_PART_INDICATOR); /* no point markers */
    lv_obj_set_style_line_width(ch, 2, LV_PART_ITEMS);
    lv_chart_series_t *s = lv_chart_add_series(ch, c, LV_CHART_AXIS_PRIMARY_Y);
    for (int i = 0; i < 60; i++) {
        int v = base + ((i * 37 + base * 7) % (amp * 2 + 1)) - amp + (slope * i) / 10;
        if (v < 2)
            v = 2;
        if (v > 98)
            v = 98;
        lv_chart_set_next_value(ch, s, v);
    }
}

/* Card: bg #141414, 1px #333, radius 4 (design 1a). */
static lv_obj_t *card_at(lv_obj_t *p, int x, int y, int w, int h) {
    lv_obj_t *c = lv_obj_create(p);
    lv_obj_set_size(c, w, h);
    lv_obj_set_pos(c, x, y);
    lv_obj_set_style_bg_color(c, C_CARD, 0);
    lv_obj_set_style_border_width(c, 1, 0);
    lv_obj_set_style_border_color(c, C_BORDER, 0);
    lv_obj_set_style_radius(c, 4, 0);
    lv_obj_set_style_pad_all(c, 0, 0);
    lv_obj_remove_flag(c, LV_OBJ_FLAG_SCROLLABLE);
    return c;
}

/* Source tag, top-right of a card ("esp32" / "wire" / "new"). Dev builds only. */
static void tag_at(lv_obj_t *card, int card_w, const char *tag) {
    if (!tag)
        return;
    lv_color_t c = C_DIMMER;
    if (!strcmp(tag, "wire"))
        c = C_BLUE;
    else if (!strcmp(tag, "new"))
        c = C_ORANGE;
    LV_UNUSED(card_w);
    lv_obj_t *l = text_at(card, 0, 0, tag, &lv_font_montserrat_14, c);
    lv_obj_align(l, LV_ALIGN_TOP_RIGHT, -16, 14);
}

/* ---- shared chrome ---- */

typedef struct {
    lv_obj_t *scr;
    lv_obj_t *content; /* full-screen layer; children use absolute coords */
} Screen;

/* Header status strip: 8 channel meters + CPU gauge (design 1a). */
static void status_strip(lv_obj_t *hdr, const int *ch_pct, int n_active, int cpu_pct) {
    /* 8 bars 8x44, gap 3, anchored right:16 */
    const int total = 8 * 8 + 7 * 3;
    int x0 = HOR - 16 - 70 - 12 - 40 - total;
    for (int i = 0; i < 8; i++) {
        int x = x0 + i * 11;
        if (i < n_active) {
            plain(hdr, x, 16, 8, 44, C_METERBG);
            int h = (44 * ch_pct[i]) / 100;
            plain(hdr, x, 16 + (44 - h), 8, h, C_GREEN);
        } else {
            plain(hdr, x, 16 + 42, 8, 2, C_STUB); /* inactive: 2px stub */
        }
    }
    char buf[24];
    snprintf(buf, sizeof(buf), "CPU %d%%", cpu_pct);
    text_at(hdr, HOR - 16 - 70 - 12 - 62, 22, buf, &lv_font_montserrat_14, C_DIM);
    bar_at(hdr, HOR - 16 - 70, 34, 70, 8, cpu_pct, cpu_pct >= 85 ? C_ORANGE : C_GREEN);
}

static void softkeys(lv_obj_t *root, const char *const *keys) {
    for (int i = 0; i < N_SOFTKEYS; i++) {
        /* 205x88 @ y626, x 8 + n*211 (design 1a) */
        lv_obj_t *b =
            plain(root, 8 + i * 211, 626, 205, 88, keys[i] && keys[i][0] ? C_BLUE : C_CARD);
        lv_obj_set_style_radius(b, 5, 0);
        if (keys[i] && keys[i][0]) {
            lv_obj_t *l = text_at(b, 0, 0, keys[i], &lv_font_montserrat_36, C_TEXT);
            lv_obj_center(l);
        }
    }
}

static Screen chrome(const char *title, const char *const *keys) {
    Screen s;
    s.scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s.scr, C_BG, 0);
    lv_obj_set_style_pad_all(s.scr, 0, 0);
    lv_obj_remove_flag(s.scr, LV_OBJ_FLAG_SCROLLABLE);
    s.content = s.scr;

    lv_obj_t *hdr = plain(s.scr, 0, 0, HOR, HEADER_H, C_HEADER);
    lv_obj_t *t = text_at(hdr, 0, 0, title, &lv_font_montserrat_36, C_TEXT);
    lv_obj_center(t);

    static const int ch[8] = {70, 62, 0, 0, 0, 0, 0, 0};
    status_strip(hdr, ch, 2, 34);

    softkeys(s.scr, keys);
    return s;
}

/* Tab bar: lv_buttonmatrix-equivalent, 5 cells 256x56 @ 0,75 (design 1a). */
static void tab_bar(lv_obj_t *root, int active) {
    static const char *tabs[5] = {"System", "Audio", "Link", "Storage", "MIDI"};
    plain(root, 0, HEADER_H, HOR, 56, C_BG);
    for (int i = 0; i < 5; i++) {
        int x = i * 256;
        lv_obj_t *cell = plain(root, x, HEADER_H, 256, 56, i == active ? C_TABON : C_BG);
        lv_obj_t *l =
            text_at(cell, 0, 0, tabs[i], &lv_font_montserrat_22, i == active ? C_TEXT : C_DIMMER);
        lv_obj_center(l);
        if (i == active) {
            plain(root, x, HEADER_H + 52, 256, 4, C_BLUE); /* 4px active edge */
        }
    }
}

/* ---- tile: the design's card anatomy ---- */

typedef struct {
    const char *k;    /* title */
    const char *tag;  /* source tag or NULL */
    const char *v;    /* value */
    const char *u;    /* unit suffix */
    const char *sub;  /* context line */
    const char *sub2; /* second context line */
    int bp;           /* gauge percent, -1 = omit */
    int warn;         /* percent at which the fill turns orange; 0 = never.
                         Not every gauge means "high is bad" - a full
                         pre-buffer or plenty of free space is healthy, and
                         colouring those orange would be decoration rather
                         than the real warning the design asks for. */
    int tgt;          /* target tick x within gauge, 0 = none */
    int spark;        /* 1 = draw sparkline */
    lv_color_t spc;
    int sbase, samp, sslope;
    int x, y, w;
    const char *mb_l[4]; /* mini bars */
    int mb_p[4];
    int mb_n;
} Tile;

static void draw_tile(lv_obj_t *root, const Tile *t) {
    const int w = t->w ? t->w : CARD_W;
    lv_obj_t *c = card_at(root, t->x, t->y, w, CARD_H);
    text_at(c, 16, 14, t->k, &lv_font_montserrat_18, C_DIM);
    tag_at(c, w, t->tag);

    /* value 36 + unit 22 on the same baseline */
    int vy = t->spark ? 48 : 64;
    lv_obj_t *v = text_at(c, 16, vy, t->v, &lv_font_montserrat_36, C_TEXT);
    if (t->u && t->u[0]) {
        /* Unit sits on the value's baseline; align_to avoids measuring text
           through lv_text_get_width, which is a private API in 9.4. */
        lv_obj_t *u = text_at(c, 0, 0, t->u, &lv_font_montserrat_22, C_DIM);
        lv_obj_align_to(u, v, LV_ALIGN_OUT_RIGHT_BOTTOM, 10, -6);
    }

    if (t->sub) {
        lv_obj_t *l = text_at(c, 16, t->spark ? 104 : 112, t->sub, &lv_font_montserrat_18, C_DIM);
        lv_obj_set_width(l, w - 32);
        lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
    }
    if (t->sub2) {
        lv_obj_t *l = text_at(c, 16, 138, t->sub2, &lv_font_montserrat_18, C_DIM);
        lv_obj_set_width(l, w - 32);
        lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
    }

    if (t->spark)
        spark_at(c, 16, 150, GAUGE_W, 56, t->spc, t->sbase, t->samp, t->sslope);

    if (t->bp >= 0) {
        const int warn = t->warn;
        lv_obj_t *b = bar_at(
            c, 16, 196, GAUGE_W, 14, t->bp, (warn > 0 && t->bp >= warn) ? C_ORANGE : C_GREEN);
        LV_UNUSED(b);
        if (t->tgt > 0) {
            /* white 2px requirement tick (design 2c) */
            plain(c, 16 + t->tgt, 192, 2, 22, C_TEXT);
        }
    }

    /* mini bars: 180x12 rows (OUTPUT L/R, SAMPLE RAM pools) */
    /* Bars go under whatever is already stacked above them: value, then
       context line. Card is 226 tall, so two rows still clear the bottom. */
    int mb_top = 60;
    if (t->v && t->v[0])
        mb_top = 118;
    if (t->sub)
        mb_top = 148;
    for (int i = 0; i < t->mb_n; i++) {
        int y = mb_top + i * 34;
        text_at(c, 16, y, t->mb_l[i], &lv_font_montserrat_18, C_DIM);
        bar_at(c, 100, y + 6, 180, 12, t->mb_p[i], C_GREEN);
    }
}

static void render(lv_display_t *disp, Screen s, const char *path) {
    lv_screen_load(s.scr);
    s_tick += 50;
    lv_timer_handler();
    lv_refr_now(disp);
    write_bmp(path);
}

static const char *const SK_DIAG[N_SOFTKEYS] = {"Back", "Tab <", "Tab >", "Freeze", "", "Reset"};

/* ================= 01 System (design 1a) ================= */
static void screen_system(lv_display_t *disp) {
    Screen s = chrome("Diagnostics", SK_DIAG);
    tab_bar(s.scr, 0);
    static const struct {
        const char *k, *v, *u;
        int p;
    } m[8] = {
        {"CPU CORE 0", "7.4", "%", 7},
        {"CPU CORE 1", "3.1", "%", 3},
        {"HEAP INTERNAL", "412", "KB free", 55},
        {"PSRAM", "26.0", "MB free", 19},
        {"LVGL POOL", "74 / 128", "KB", 58},
        {"UI FRAME TIME", "11", "ms", 33},
        {"UPTIME", "2h 41m", "", -1},
        {"MIN FREE HEAP", "233", "KB", -1},
    };
    for (int i = 0; i < 8; i++) {
        Tile t = {0};
        t.k = m[i].k;
        t.v = m[i].v;
        t.u = m[i].u;
        t.bp = m[i].p; /* -1 omits the bar: no empty tracks (design 1a) */
        t.warn = 85;   /* consumption gauges: high really is bad */
        t.x = 12 + (i % 4) * 317;
        t.y = 143 + (i / 4) * 238;
        draw_tile(s.scr, &t);
    }
    render(disp, s, "out/01_diag_system.bmp");
}

/* ================= 02 Audio (design 2a) ================= */
static void screen_audio(lv_display_t *disp) {
    Screen s = chrome("Diagnostics", SK_DIAG);
    tab_bar(s.scr, 1);

    Tile t[8] = {0};
    for (int i = 0; i < 8; i++) {
        t[i].bp = -1;
        t[i].x = 12 + (i % 4) * 317;
        t[i].y = 143 + (i / 4) * 238;
    }
    t[0] = (Tile){.k = "CALLBACK RATE",
                  .tag = "wire",
                  .v = "1000",
                  .u = "Hz",
                  .sub = "expect 1000 - engine alive",
                  .bp = -1,
                  .spark = 1,
                  .spc = C_GREEN,
                  .sbase = 50,
                  .samp = 3,
                  .sslope = 0,
                  .x = 12,
                  .y = 143};
    t[1] = (Tile){.k = "RING LOW-WATER",
                  .tag = "wire",
                  .v = "180",
                  .u = "/ 2048 fr",
                  .sub = "min this interval",
                  .bp = -1,
                  .spark = 1,
                  /* trending down >=20% over window -> orange (design 2a) */
                  .spc = C_ORANGE,
                  .sbase = 70,
                  .samp = 8,
                  .sslope = -9,
                  .x = 329,
                  .y = 143};
    t[2] = (Tile){.k = "UNDERRUNS",
                  .tag = "wire",
                  .v = "0",
                  .u = "/ min",
                  .sub = "episodes, not raw events",
                  .bp = -1,
                  .spark = 1,
                  .spc = C_GREEN,
                  .sbase = 4,
                  .samp = 1,
                  .sslope = 0,
                  .x = 646,
                  .y = 143};
    t[3] = (Tile){.k = "ENGINE CPU",
                  .tag = "esp32",
                  .v = "34 / 41",
                  .u = "% avg / max",
                  .bp = -1,
                  .spark = 1,
                  .spc = C_GREEN,
                  .sbase = 34,
                  .samp = 4,
                  .sslope = 0,
                  .x = 963,
                  .y = 143};
    t[4] = (Tile){.k = "OUTPUT L / R",
                  .tag = "esp32",
                  .v = "",
                  .bp = -1,
                  .x = 12,
                  .y = 381,
                  .mb_n = 4,
                  .mb_l = {"PK L", "PK R", "RMS L", "RMS R"},
                  .mb_p = {70, 66, 40, 38}};
    t[5] = (Tile){.k = "CURRENT WAV",
                  .tag = "wire",
                  .v = "44.1k",
                  .u = "16-bit stereo",
                  .sub = "resample x1.000 - 0 push/s",
                  .bp = -1,
                  .x = 329,
                  .y = 381};
    t[6] = (Tile){.k = "PRE-BUFFER",
                  .tag = "wire",
                  .v = "1024",
                  .u = "/ 1024",
                  .sub = "fill",
                  .bp = 100,
                  .x = 646,
                  .y = 381};
    t[7] = (Tile){.k = "VOICES",
                  .tag = "new",
                  .v = "3",
                  .u = "active",
                  .sub = "0 discarded passes/s",
                  .bp = -1,
                  .x = 963,
                  .y = 381};
    for (int i = 0; i < 8; i++)
        draw_tile(s.scr, &t[i]);
    render(disp, s, "out/02_diag_audio.bmp");
}

/* ================= 03 Link (design 2b) ================= */
static void screen_link(lv_display_t *disp) {
    Screen s = chrome("Diagnostics", SK_DIAG);
    tab_bar(s.scr, 2);

    Tile t[4] = {0};
    t[0] = (Tile){.k = "LINK",
                  .tag = "esp32",
                  .v = "OK",
                  .sub = "heartbeat 0.4 s - RTT 2.1 ms",
                  .bp = -1,
                  .x = 12,
                  .y = 143};
    t[1] = (Tile){.k = "FRAMES / S",
                  .tag = "wire",
                  .v = "1000 / 998",
                  .u = "RX / TX",
                  .sub = "96/88 KB/s of 200 ceiling",
                  .bp = 48,
                  .warn = 85,
                  .x = 329,
                  .y = 143};
    t[2] = (Tile){.k = "LINK CPU",
                  .tag = "wire",
                  .v = "412",
                  .u = "us / interval",
                  .sub = "8.2% of loop",
                  .bp = -1,
                  .spark = 1,
                  .spc = C_GREEN,
                  .sbase = 41,
                  .samp = 3,
                  .sslope = 0,
                  .x = 12,
                  .y = 381};
    t[3] = (Tile){.k = "ERRORS / MIN",
                  .tag = "esp32",
                  .v = "0",
                  .u = "crc/sync/ovfl",
                  .sub = "seq drops 0 - resyncs 0",
                  .bp = -1,
                  .x = 329,
                  .y = 381};
    for (int i = 0; i < 4; i++)
        draw_tile(s.scr, &t[i]);

    /* Per-message-type table 622x464 @ 646,143 - free from wavex_packet_stats_t */
    lv_obj_t *panel = card_at(s.scr, 646, 143, 622, 464);
    text_at(panel, 16, 14, "MESSAGE", &lv_font_montserrat_18, C_DIM);
    text_at(panel, 380, 14, "RX", &lv_font_montserrat_18, C_DIM);
    text_at(panel, 500, 14, "TX", &lv_font_montserrat_18, C_DIM);
    static const char *rows[12][3] = {
        {"HEARTBEAT", "120", "-"},
        {"AUDIO_PEAKS", "3600", "-"},
        {"SEQ_PLAYHEAD", "480", "-"},
        {"TRANSPORT", "2", "4"},
        {"NOTE_ON", "-", "842"},
        {"NOTE_OFF", "-", "840"},
        {"CC", "-", "96"},
        {"STORAGE_STATUS", "12", "-"},
        {"SAMPLE_MEM", "12", "-"},
        {"FILE_PAGE", "4", "4"},
        {"PERF", "24", "-"},
        {"ACK", "-", "18"},
    };
    for (int i = 0; i < 12; i++) {
        int y = 52 + i * 32;
        text_at(panel, 16, y, rows[i][0], &lv_font_montserrat_18, C_TEXT);
        text_at(panel, 380, y, rows[i][1], &lv_font_montserrat_18, C_DIM);
        text_at(panel, 500, y, rows[i][2], &lv_font_montserrat_18, C_DIM);
    }
    /* styled scrollbar: 4px track/thumb */
    plain(panel, 614, 52, 4, 384, C_TRACK);
    plain(panel, 614, 52, 4, 180, C_GREEN);

    render(disp, s, "out/03_diag_link.bmp");
}

/* ================= 04 Storage (design 2c) ================= */
static void screen_storage(lv_display_t *disp) {
    Screen s = chrome("Diagnostics", SK_DIAG);
    tab_bar(s.scr, 3);

    Tile t[4] = {0};
    t[0] = (Tile){.k = "THROUGHPUT",
                  .tag = "wire",
                  .v = "212",
                  .u = "KB/s",
                  .sub = "required 176 KB/s",
                  .bp = 71,
                  .tgt = (273 * 176) / 300, /* white requirement tick */
                  .x = 12,
                  .y = 143};
    t[1] = (Tile){.k = "READ LATENCY",
                  .tag = "wire",
                  .v = "380 / 2100",
                  .u = "us",
                  .sub = "avg / max",
                  .bp = -1,
                  .spark = 1,
                  .spc = C_GREEN,
                  .sbase = 40,
                  .samp = 6,
                  .sslope = 0,
                  .x = 329,
                  .y = 143};
    t[2] = (Tile){.k = "READ ERRORS",
                  .tag = "wire",
                  .v = "0",
                  .u = "/ interval",
                  .sub = "0 recovered - FatFS OK - HAL OK",
                  .bp = -1,
                  .x = 646,
                  .y = 143};
    t[3] = (Tile){.k = "CARD",
                  .tag = "wire",
                  .v = "SDHC 32 GB",
                  .sub = "mounted - 40 MHz bus",
                  .bp = -1,
                  .x = 963,
                  .y = 143};
    for (int i = 0; i < 4; i++)
        draw_tile(s.scr, &t[i]);

    Tile f = {.k = "CURRENT FILE",
              .tag = "wire",
              .v = "amen-full.wav",
              .sub = "data_start 44 - aligned - backoff off - loop-point 2048",
              .bp = -1,
              .x = 12,
              .y = 381,
              .w = 622};
    draw_tile(s.scr, &f);

    Tile ram = {.k = "SAMPLE RAM",
                .tag = "wire",
                .v = "0",
                .u = "failed allocs",
                .sub = "largest block 1.2 MB",
                .bp = -1,
                .x = 646,
                .y = 381,
                .mb_n = 2,
                .mb_l = {"SMALL", "LARGE"},
                .mb_p = {62, 41}};
    draw_tile(s.scr, &ram);

    Tile fs = {
        .k = "FREE SPACE", .tag = "wire", .v = "12.4", .u = "GB", .bp = 61, .x = 963, .y = 381};
    draw_tile(s.scr, &fs);

    render(disp, s, "out/04_diag_storage.bmp");
}

/* ================= 05 MIDI (design 2d) ================= */
static void screen_midi(lv_display_t *disp) {
    Screen s = chrome("Diagnostics", SK_DIAG);
    tab_bar(s.scr, 4);

    Tile t[8] = {0};
    t[0] = (Tile){.k = "SYNC",
                  .tag = "wire",
                  .v = "LOCKED",
                  .sub = "external - freewheel armed",
                  .bp = -1,
                  .x = 12,
                  .y = 143};
    t[1] = (Tile){.k = "BPM",
                  .tag = "wire",
                  .v = "128.02",
                  .u = "measured",
                  .bp = -1,
                  .spark = 1,
                  .spc = C_GREEN,
                  .sbase = 50,
                  .samp = 2,
                  .sslope = 0,
                  .x = 329,
                  .y = 143};
    t[2] = (Tile){.k = "CLOCK TICKS",
                  .tag = "new",
                  .v = "51.2",
                  .u = "/ s",
                  .sub = "expected 51.2 - jitter +-0.3%",
                  .bp = -1,
                  .x = 646,
                  .y = 143};
    t[3] = (Tile){.k = "TRANSPORT",
                  .tag = "wire",
                  .v = "PLAY",
                  .sub = "pattern A3 - step 09/16 - loop on",
                  .bp = -1,
                  .x = 963,
                  .y = 143};
    t[4] = (Tile){.k = "NOTES / CC",
                  .tag = "new",
                  .v = "14 / 3",
                  .u = "per s",
                  .bp = -1,
                  .spark = 1,
                  .spc = C_GREEN,
                  .sbase = 30,
                  .samp = 14,
                  .sslope = 0,
                  .x = 12,
                  .y = 381};
    t[5] = (Tile){.k = "LAST NOTE",
                  .tag = "new",
                  .v = "C3 - vel 96",
                  .sub = "ch 10 - 0.8 s ago",
                  .bp = -1,
                  .x = 329,
                  .y = 381};
    t[6] = (Tile){.k = "LAST CC",
                  .tag = "new",
                  .v = "74 > 52",
                  .sub = "ch 1 - 2.1 s ago",
                  .bp = -1,
                  .x = 646,
                  .y = 381};
    t[7] = (Tile){.k = "DROPPED / LATE",
                  .tag = "new",
                  .v = "0 / 0",
                  .u = "/ interval",
                  .sub = "resets on read",
                  .bp = -1,
                  .x = 963,
                  .y = 381};
    for (int i = 0; i < 8; i++)
        draw_tile(s.scr, &t[i]);
    render(disp, s, "out/05_diag_midi.bmp");
}

/* ---- waveform on lv_canvas (the widget the firmware already uses) ---- */
static uint8_t canvas_buf[LV_CANVAS_BUF_SIZE(1256, 250, 16, LV_DRAW_BUF_STRIDE_ALIGN)];

static lv_obj_t *waveform(
    lv_obj_t *p, int x, int y, int w, int h, int start_pct, int end_pct, int playhead_pct) {
    lv_obj_t *cv = lv_canvas_create(p);
    lv_canvas_set_buffer(cv, canvas_buf, w, h, LV_COLOR_FORMAT_RGB565);
    lv_obj_set_pos(cv, x, y);
    lv_canvas_fill_bg(cv, lv_color_hex(0x0A0A0A), LV_OPA_COVER);

    const int mid = h / 2;
    const int sx = (w * start_pct) / 100;
    const int ex = (w * end_pct) / 100;
    for (int px = 0; px < w; px++) {
        /* deterministic pseudo-waveform envelope */
        int e = (px * 7919) % 211;
        int amp = (mid - 6) * (40 + (e % 60)) / 100;
        if ((px % 97) < 12)
            amp = (amp * 3) / 2;
        if (amp > mid - 4)
            amp = mid - 4;
        int inside = (px >= sx && px <= ex);
        lv_color_t c = inside ? C_GREEN : lv_color_hex(0x1E2A20);
        for (int dy = -amp; dy <= amp; dy++) {
            lv_canvas_set_px(cv, px, mid + dy, c, LV_OPA_COVER);
        }
    }
    /* start / end markers */
    for (int dy = 0; dy < h; dy++) {
        lv_canvas_set_px(cv, sx, dy, C_GREEN, LV_OPA_COVER);
        lv_canvas_set_px(cv, sx + 1, dy, C_GREEN, LV_OPA_COVER);
        lv_canvas_set_px(cv, ex, dy, C_ORANGE, LV_OPA_COVER);
        lv_canvas_set_px(cv, ex + 1, dy, C_ORANGE, LV_OPA_COVER);
    }
    if (playhead_pct >= 0) {
        int ph = (w * playhead_pct) / 100;
        for (int dy = 0; dy < h; dy++)
            lv_canvas_set_px(cv, ph, dy, C_DIM, LV_OPA_COVER);
    }
    return cv;
}

/* ================= 06 Sample browser (design 1b) ================= */
static void screen_browser(lv_display_t *disp) {
    static const char *const sk[N_SOFTKEYS] = {"Back", "Stop", "Load", "Edit", "Sort", "Eject"};
    Screen s = chrome("Samples  /drums", sk);

    /* status strip 770x30 @ 12,87 */
    text_at(s.scr, 12, 87, "1-20 of 63  -  name ^", &lv_font_montserrat_18, C_DIM);
    text_at(s.scr, 620, 87, "SD 12.4 GB free", &lv_font_montserrat_18, C_DIM);

    /* file list 770x487 @ 12,121 */
    lv_obj_t *list = plain(s.scr, 12, 121, 770, 487, C_LISTBG);
    lv_obj_set_style_border_width(list, 1, 0);
    lv_obj_set_style_border_color(list, C_BORDER, 0);

    static const struct {
        const char *n, *d;
        int dir, sel;
    } rows[7] = {
        {"..", "", 1, 0},
        {"808-kick.wav", "0:01", 0, 0},
        {"amen-full.wav", "0:07", 0, 1},
        {"clap-tight.wav", "0:01", 0, 0},
        {"hat-open.wav", "0:02", 0, 0},
        {"ride-bell.wav", "0:03", 0, 0},
        {"snare-909.wav", "0:01", 0, 0},
    };
    for (int i = 0; i < 7; i++) {
        int y = i * 54;
        lv_obj_t *row = plain(list, 1, y, 766, 54, rows[i].sel ? C_SELBG : C_LISTBG);
        if (rows[i].sel) {
            lv_obj_set_style_border_width(row, 2, 0);
            lv_obj_set_style_border_color(row, C_GREEN, 0);
            lv_obj_set_style_radius(row, 4, 0);
        }
        text_at(row, 14, 14, rows[i].n, &lv_font_montserrat_22, rows[i].dir ? C_DIM : C_TEXT);
        if (rows[i].d[0]) {
            lv_obj_t *d =
                text_at(row, 0, 0, rows[i].d, &lv_font_montserrat_18, lv_color_hex(0x7F8A90));
            lv_obj_align(d, LV_ALIGN_RIGHT_MID, -16, 0);
        }
    }
    /* loading row: never a blank list while a page is in flight (design 1b) */
    lv_obj_t *loading = plain(list, 1, 7 * 54, 766, 54, C_LISTBG);
    lv_obj_t *sp = lv_spinner_create(loading);
    lv_obj_set_size(sp, 24, 24);
    lv_obj_set_pos(sp, 14, 15);
    lv_obj_set_style_arc_color(sp, C_GREEN, LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(sp, 3, LV_PART_MAIN);
    lv_obj_set_style_arc_width(sp, 3, LV_PART_INDICATOR);
    text_at(loading, 50, 16, "Loading 21-40...", &lv_font_montserrat_18, C_DIM);

    /* scrollbar 4px */
    plain(list, 764, 4, 4, 479, C_TRACK);
    plain(list, 764, 4, 4, 150, C_GREEN);

    /* detail panel 474x521 @ 794,87 */
    lv_obj_t *det = plain(s.scr, 794, 87, 474, 521, C_PANEL);
    lv_obj_set_style_border_width(det, 1, 0);
    lv_obj_set_style_border_color(det, C_BORDER, 0);
    text_at(det, 16, 14, "amen-full.wav", &lv_font_montserrat_26, C_TEXT);
    waveform(det, 16, 56, 442, 150, 0, 100, -1);

    static const char *meta[3][2] = {
        {"Format", "44.1 kHz - 16-bit - stereo"},
        {"Length", "0:07.42 - 1.3 MB"},
        {"Data", "offset 44 (aligned)"},
    };
    for (int i = 0; i < 3; i++) {
        int y = 222 + i * 34;
        text_at(det, 16, y, meta[i][0], &lv_font_montserrat_18, C_DIM);
        text_at(det, 136, y, meta[i][1], &lv_font_montserrat_18, C_TEXT);
        if (i < 2)
            plain(det, 16, y + 30, 442, 1, lv_color_hex(0x222222));
    }

    text_at(det, 16, 458, "AUDITIONING  -  0:03 / 0:07", &lv_font_montserrat_18, C_GREEN);
    bar_at(det, 16, 490, 442, 10, 42, C_GREEN);

    render(disp, s, "out/06_sample_browser.bmp");
}

/* ================= 07 Sample edit (design 2e) ================= */
static void screen_edit(lv_display_t *disp) {
    static const char *const sk[N_SOFTKEYS] = {
        "Back", "Audition", "Zoom -", "Zoom +", "Normalize", "Save"};
    Screen s = chrome("Edit  amen-full.wav", sk);

    /* Save disabled until an edit exists (design 2e) */
    lv_obj_t *save = plain(s.scr, 8 + 5 * 211, 626, 205, 88, C_CARD);
    lv_obj_set_style_radius(save, 5, 0);
    lv_obj_t *sl = text_at(save, 0, 0, "Save", &lv_font_montserrat_36, C_DIMMER);
    lv_obj_center(sl);

    waveform(s.scr, 12, 87, 1256, 250, 5, 93, 34);

    /* marker handles: 30x26, S on start (green) / E on end (orange) */
    {
        struct {
            int x;
            lv_color_t c;
            const char *l;
        } hs[2] = {
            {12 + (1256 * 5) / 100, C_GREEN, "S"},
            {12 + (1256 * 93) / 100, C_ORANGE, "E"},
        };
        for (int i = 0; i < 2; i++) {
            lv_obj_t *hd = plain(s.scr, hs[i].x - 15, 87, 30, 26, hs[i].c);
            lv_obj_t *hl = text_at(hd, 0, 0, hs[i].l, &lv_font_montserrat_18, C_BG);
            lv_obj_center(hl);
        }
    }

    /* param strip: 4 cells 305x132 @ y353, x 12+n*317 */
    static const struct {
        const char *k, *v;
        int slider, p, foc;
    } prm[4] = {
        {"START", "0:00.312", 1, 5, 0},
        {"END", "0:06.918", 1, 93, 0},
        {"GAIN", "+2.5 dB", 1, 62, 1},
        {"LOOP", "Off", 0, 0, 0},
    };
    for (int i = 0; i < 4; i++) {
        lv_obj_t *c = card_at(s.scr, 12 + i * 317, 353, CARD_W, 132);
        if (prm[i].foc) { /* encoder focus ring */
            lv_obj_set_style_border_width(c, 2, 0);
            lv_obj_set_style_border_color(c, C_BLUE, 0);
        }
        text_at(c, 16, 12, prm[i].k, &lv_font_montserrat_18, C_DIM);
        text_at(c, 16, 40, prm[i].v, &lv_font_montserrat_32, C_TEXT);
        if (prm[i].slider) {
            lv_obj_t *b = bar_at(c, 16, 96, GAUGE_W, 14, prm[i].p, C_BLUE);
            LV_UNUSED(b);
            plain(c, 16 + (GAUGE_W * prm[i].p) / 100 - 4, 92, 8, 22, C_TEXT);
        } else {
            text_at(c, 16 + 60, 44, "v", &lv_font_montserrat_22, C_DIM);
        }
    }

    /* info strip 1256x88 @ 12,509 */
    lv_obj_t *info = card_at(s.scr, 12, 509, 1256, 88);
    text_at(info,
            16,
            14,
            "selection 0:06.606  -  291,325 frames  -  zoom 1:4",
            &lv_font_montserrat_18,
            C_DIM);
    text_at(info, 16, 48, "audition 0:02.41", &lv_font_montserrat_18, C_GREEN);
    bar_at(info, 820, 40, 400, 10, 34, C_GREEN);

    render(disp, s, "out/07_sample_edit.bmp");
}

int main(void) {
    lv_init();
    lv_tick_set_cb(tick_cb);

    lv_display_t *disp = lv_display_create(HOR, VER);
    lv_display_set_buffers(disp, fb, NULL, sizeof(fb), LV_DISPLAY_RENDER_MODE_DIRECT);
    lv_display_set_flush_cb(disp, flush_cb);

    screen_system(disp);
    screen_audio(disp);
    screen_link(disp);
    screen_storage(disp);
    screen_midi(disp);
    screen_browser(disp);
    screen_edit(disp);
    return 0;
}
