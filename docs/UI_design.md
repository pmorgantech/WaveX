# Polyphonic Sampler – Menu‑System Design (480 × 320 touch display)

## 1. Design Goals

* **Fast to navigate** on stage: two taps or one encoder push should reach any key function.
* **Consistent visual language** using LVGL widgets (tabview + stack of modals).
* **Non‑blocking UI** – every screen renders from a shadow framebuffer, updated in ≤16 ms.
* **Hardware‑agnostic back‑end** – same menu code runs in simulator (SDL) and on ESP32‑S3.
* **Safe persistence** – all edits cached in RAM, committed atomically to SD.

---

## 2. Input & Navigation Paradigm

| Control                   | Action                                                             |
| ------------------------- | ------------------------------------------------------------------ |
| **Touch‑screen**          | Primary pointer. Short‑tap selects, long‑press opens context menu. |
| **Encoder A** (with push) | Global focus wheel → scroll / change value; push = OK.             |
| **Encoder B**             | Zoom in editors (waveform, envelopes).                             |
| **Back button**           | Always returns to parent page.                                     |
| **DIN/USB MIDI**          | Program‑change = patch load, CC #0‑31 reserved for quick params.   |

> *Hint:* Keep tactile controls functional even if the touch panel needs recalibration mid‑gig.

---

## 3. Screen Layout

```
┌──────────────────────────────────────────────┐
│  Top bar (24 px)  –  mode • patch • BPM • CPU│
├──────────────────────────────────────────────┤
│                                              │
│          Dynamic content area (272 px)       │
│                                              │
├──────────────────────────────────────────────┤
│  Bottom bar (24 px) – soft‑keys / hints      │
└──────────────────────────────────────────────┘
```

*Top bar* rendered by a dedicated LVGL layer, updated @ 2 Hz.
*Soft‑keys* change per page; icons mirror encoder push‑function.

---

## 4. Menu Hierarchy (two‑depth, max)

```
Main Menu
├─ Play/Perform
│   ├─ Live tweaks (pots/encoders)
│   └─ Mod matrix view
├─ Sample Mode
│   ├─ Arm & Threshold
│   ├─ Duration / SR   
│   └─ Take list
├─ Sample Editor
│   ├─ Trim & Zoom
│   ├─ Normalize
│   └─ Loop Points
├─ Patch Manager
│   ├─ Load / Save / Rename
│   └─ Category filter
├─ MIDI & Sync
│   ├─ DIN ⇆ USB routing
│   ├─ Clock source & tempo
│   └─ Channel map
├─ System Settings
│   ├─ Audio (rate/latency)
│   ├─ Storage (SD, MSC)
│   ├─ Firmware update
│   └─ UI theme
├─ Diagnostics
│   ├─ CPU/RAM/PSRAM graphs
│   ├─ SD‑bench & flash wear
│   ├─ ADC visualize
│   └─ MIDI monitor
└─ Calibration
    ├─ Touch grid (5‑point)
    └─ ADC auto‑scale (pots & CV)
```

---

## 5. Page‑by‑Page Details

### 5.1 Play/Perform

* **Overview tile grid**: voice meters, current patch name, macro knobs.
* **Encoder A** rotates through pages (Pitch, Filter, Amp, Modulation).
* **Touch drag** on a macro opens detailed ADSR/LFO editor in modal.

### 5.2 Sample Mode

* **Arm** button turns red; volume meter runs (DMA to LVGL chart).
* **Auto‑Threshold** slider – real‑time horizontal line; crossing triggers.
* **Duration** knob (0.1–60 s).
* **Rate** selector: 44.1 k, 22.05 k, 11.025 k.
* After capture, jumps to *Sample Editor* with new take pre‑selected.

### 5.3 Sample Editor

* Waveform widget (lv\_chart + custom draw\_cb) with two draggable cursors A/B.
* *Trim* → crops between cursors. *Normalize* → peak to ‑1 dBFS.
* *Loop* sub‑page with dual cursors and cross‑fade knob.
* Encoder B zooms horizontally; pinch gesture supported.

### 5.4 Patch Manager

* Virtual keyboard for naming.
* SD directory tree in LVGL `lv_file_explorer` style list.
* **Auto‑backup** old version on save (timestamp suffix).

### 5.5 MIDI & Sync

* Matrix of check‑boxes: {DIN In, USB In, Soft‑Thru} × {Engine, DIN Out, USB Out}.
* Clock section: Internal / DIN / USB; tempo knob greyed when external.
* **Learn CC** modal: move control → mapping stored.

### 5.6 System Settings

* Audio block size selector (128–1024 frames). Shows resulting latency.
* QSPI flash wear‑level indicator (progress bar).
* Theme picker (dark/light, accent color).

### 5.7 Diagnostics

* Live **heap chart** (lv\_chart) every 500 ms.
* **SD read‑speed** button runs test, dumps MB/s.
* **ADC scope**: traces 8 channels @ 1 kHz.
* **MIDI monitor**: running log with timestamps.

### 5.8 Calibration

* **Touch**: Five‑point wizard writes to NVS.
* **ADC**: Prompts user to turn each pot from min to max; stores slope/offset.

---

## 6. Task & Module Architecture

```
                         ┌────────────┐
                         │  Engine    │ (Daisy or Xtensa core)
                         └────┬───────┘
       SPI (CS‑assert)        │  Sample RAM
┌──────────┐  Q   ┌──────────┐│
│  UI Core │◄────►│  MsgBus  │◄───────── MIDI ISR / SD DMA / ADC DMA
└──────────┘  S   └──────────┘
      ▲ Framebuffer   │
      └─ LVGL Render ─┘
```

* **MsgBus** (ring‑buffer of structs) decouples RT tasks from UI.
* **UI Core** uses LVGL task handler in its own FreeRTOS thread pinned to core 0.
* **Engine** audio runs on core 1 with IRAM‑cached routines.

---

## 7. LVGL Implementation Notes

* Use **`lv_tabview`** for Main Menu; tabs are icons only, title shown in top bar.
* Pages are `lv_obj` with flex column layout; widgets created once, values updated.
* All images (icons, fonts) converted with 4‑bit RLE to fit PSRAM.
* Waveform widget: allocate vertex array in `heap_caps_malloc(…, MALLOC_CAP_SPIRAM)`.

---

## 8. Future Expansion Hooks

* **Network page** (Wi‑Fi song update) – reserved tab index 8.
* **Granular editor** – share Sample Editor code, add additional cursor pairs.
* **OSC/NRPN** remote control – reuse MIDI monitor infrastructure.

---

## 9. Quick Dev Tips

1. **Sim first**: Use `lv_sim_sdl` at 960×640 double‑size for hi‑DPI preview.
2. **Hot reload fonts** by regenerating C arrays with `lv_font_conv` via `idf.py python`.
3. **Humour**: add an Easter‑egg in Diagnostics → tap CPU graph 7× to reveal *Tux racing the DSP* frame‑anim.

---

*Last revision: 2025‑07‑07*
