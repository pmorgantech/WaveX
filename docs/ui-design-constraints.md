# WaveX UI Design Constraints

A one-page brief for design work (human or AI). Everything here is verified
against the code as of 2026-08-28 — file references inline so it can be
re-verified when things change. If a design conflicts with this page, the
design loses.

Companion docs: [`ui-architecture.md`](ui-architecture.md) (how the UI code is
structured), [`ui-system-implementation-guide.md`](ui-system-implementation-guide.md)
(how to build a page).

## The paste-ready brief

> Design screens for a hardware groovebox/sampler with a **5-inch 1280×720
> landscape touchscreen** (720×1280 panel, software-rotated 90°), rendered
> with **LVGL 9.4** at **RGB565** (16-bit color, no alpha-heavy effects).
>
> **Fixed chrome, not negotiable:** a 75 px header strip (screen title) at the
> top and a 100 px softkey bar at the bottom with **exactly 6 equal-width
> buttons**. Softkey labels are the page's primary actions and can change with
> state (e.g. "Audition" ↔ "Stop"). The usable content area is therefore
> **1280×545 px**.
>
> **Input model:** capacitive touch, plus a **rotary encoder** that moves
> focus between softkeys/list items and clicks to activate, plus a hardware
> keypad. Every interaction must be reachable by encoder alone — touch is an
> accelerator, not a requirement. No hover states, no gestures beyond tap and
> scroll, no multi-touch.
>
> **Typography:** Montserrat only, at these compiled-in sizes: 14, 18, 22, 24,
> 26, 28, 32, 36. Body text is 18, titles 26, header/softkeys 36. No other
> fonts or sizes exist on the device.
>
> **Color:** dark theme on black. Existing palette: background #000000, header
> #2E3440, borders #333333, text #FFFFFF, accent blue #2196F3 (buttons),
> green #4CAF50 (selection, meters), orange #FF5722 (peaks/warnings). Designs
> may extend this palette but gradients should be used sparingly (RGB565 bands
> visibly on smooth gradients).
>
> **Rendering budget:** the display flushes in 20-line strips from a small
> DMA buffer and rotation is done in software, so **large animated regions
> are expensive**. UI runs at 30 FPS max. Fine for: audio meters (already
> update at 30 FPS), progress bars, list scrolling, small spinners. Avoid:
> full-screen animations, parallax, large moving images, video-like effects,
> per-frame full redraws.
>
> **Build from these LVGL widgets** (all enabled in this firmware): label,
> button, button-matrix, bar, slider, arc, chart, table, list, roller,
> dropdown, checkbox, switch, spinner, canvas (for custom drawing like
> waveforms — one already exists for waveform preview). Custom-drawn widgets
> are possible but each one is C code someone must write and maintain —
> prefer composing standard widgets.
>
> **Screens are stack-navigated**: pages push/pop with the header title
> updating; "Back" is conventionally softkey 1. Widgets are recreated on
> every page entry, so designs should not depend on preserved off-screen
> state.
>
> **Data freshness:** values from the audio engine (meters, playback state,
> file lists) arrive asynchronously and update at most at 30 FPS; file
> listings arrive in pages of 20 entries and may take visible time — lists
> need a "loading" presentation, not a blank flash.

## Where each claim comes from

| Claim | Source |
|---|---|
| 720×1280 panel, 5-inch, MIPI DSI | `CONFIG_BSP_LCD_TYPE_720_1280_5_INCH_A` in `firmware/esp32/sdkconfig`; Waveshare ESP32-P4 Nano BSP |
| Software rotation to landscape | `lv_display_set_rotation(display_, LV_DISPLAY_ROTATION_90)` + `.sw_rotate = true`, `display_manager.cpp` |
| LVGL 9.4, RGB565 | `main/idf_component.yml` (`lvgl/lvgl: >=9.4,<10`), `CONFIG_LV_COLOR_DEPTH=16` |
| 20-line strip buffer, DMA, internal RAM | `.buffer_size = 720 * 20, .double_buffer = true, .buff_dma = true, .buff_spiram = false`, `display_manager.cpp` |
| Header 75 px / softkeys 100 px / 6 buttons | `UI_HEADER_HEIGHT`, `UI_HOTKEY_HEIGHT` in `components/ui/styles/ui_theme.h`; `NUM_SOFTKEYS = 6` in `ui_softkey.h` |
| Fonts | `CONFIG_LV_FONT_MONTSERRAT_*` in `sdkconfig`; role mapping in `ui_theme.h` |
| Palette | `UI_COLOR_*` in `ui_theme.h` |
| 30 FPS cap | `vTaskDelay(pdMS_TO_TICKS(32))` in `main/ui_task.cpp` |
| Encoder + touch + keypad | `InputDispatcher`, `SoftkeyBar::focusNext/Prev`, GT911 touch, TCA8418 keypad component |
| Pages recreated on entry | `ui-architecture.md` "Known Limitations" #2 |
| Browse pagination, 20/page | `file_browser.cpp` (`entries_per_page = 20`) |

## Known discrepancy (do not design around it — it should be fixed)

The vendored BSP's `bsp_touch_new()` (`esp32_p4_nano.c`, in
`managed_components/waveshare__esp32_p4_nano/`) configures the GT911 touch
controller with `x_max = 720, y_max = 1280` — the panel's **native**
orientation — while LVGL draws to the software-rotated 1280×720 landscape
canvas (`LV_DISPLAY_ROTATION_90` in `display_manager.cpp`). Touch and
display disagree about which axis is which. If touch positions feel offset,
swapped, or compressed, this is the first suspect; verify by tapping the
four corners. Tracked in `docs/backlog.md`.

## Iterating with Claude Design

1. Paste the brief above, plus a screenshot or description of the current
   screen, plus what is wrong with it.
2. Ask for the design as a **wireframe with px dimensions and widget names
   from the list above** — not a bitmap mockup. A design expressed as "bar
   widget, 400×24, x=40 y=180, green #4CAF50 on #333333 track" translates to
   LVGL directly; a Figma-style rendering does not.
3. Reject anything that needs per-frame animation of large areas, fonts
   outside the list, alpha compositing, or more than 6 bottom actions.
