# WaveX UI Design Constraints

A one-page brief for design work (human or AI). Everything here is verified
against the code as of 2026-09-05 — file references inline so it can be
re-verified when things change. If a design conflicts with this page, the
design loses.

Companion doc: [`ui-architecture.md`](ui-architecture.md) — how the UI code is
structured, and how to build a page.

## The paste-ready brief

> Design screens for a hardware groovebox/sampler with a **5-inch 1280×720
> landscape touchscreen** (720×1280 panel, software-rotated 90°), rendered
> with **LVGL 9.5** at **RGB565** (16-bit color, no alpha-heavy effects).
>
> **Fixed chrome, not negotiable:** a 64 px header (page title left, then the
> page's context line; output meters, engine-CPU readout and the SHIFT chip
> anchored right), a 3 px rule beneath it that turns shift-coloured while
> Shift is latched, and a 96 px softkey bar at the bottom holding **exactly 6
> equal-width cards**. Softkey labels are the page's primary actions and can
> change with state (e.g. "Audition" ↔ "Stop"). An action that is unavailable
> leaves its cell empty; one that exists but is disabled stays visible and
> dimmed. Either way the row never reflows. The usable content area is
> therefore **1280×557 px**.
>
> **Input model:** capacitive touch, plus a **rotary encoder** that moves
> page-defined selection/values and clicks to activate, plus a hardware
> keypad whose SOFT keys invoke the displayed actions. There is no encoder
> focus traversal of the softkey bar. Preserve panel access to interactions. No hover states or pinch/rotate gestures. Independent touches on separate
> controls are supported (up to five contacts); two-finger panel verification
> remains open in the roadmap.
>
> **Typography:** two faces, eleven sizes, and nothing else exists on the
> device — naming another size is a link error. Montserrat for prose at 14
> (micro), 18 (small), 22 (body), 26 (title), 30 (heading: page title, row
> titles, softkey labels) and 36. JetBrains Mono for anything read as a number at 14,
> 18, 26, 38 and 48 (hero values). Use the mono face for
> values that update live: it is tabular, so a changing digit does not shift
> the widgets beside it. Roles are named `UI_FONT_*` in `ui_theme.h`; snap a
> design to the nearest existing step rather than adding a font table.
>
> **Color:** a dark theme built from named roles, not literals — background,
> card, card-alt, line, foreground, dim, dimmer, accent, accent-foreground,
> ok, warn, shift, error. Four palettes (`neutral`, `amber`, `teal`,
> `contrast`) are selected at compile time with
> `-DWAVEX_UI_THEME=<name>`; they re-map colours only, so a design that fits
> one fits all four. Design against the roles, never a hex value. Gradients
> should be used sparingly (RGB565 bands visibly on smooth gradients).
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
> dropdown, checkbox, switch, spinner, line, image, canvas (for custom drawing
> like waveforms — one already exists for waveform preview), tabview. Custom-
> drawn widgets are possible but each one is C code someone must write and
> maintain — prefer composing standard widgets. `lv_line` in particular is
> enabled and is the cheap way to draw an envelope or response curve; it does
> not need a canvas.
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
| LVGL 9.5.0, RGB565 | `main/idf_component.yml` pins `lvgl/lvgl: >=9.4,<10`; `firmware/esp32/dependencies.lock` resolves **9.5.0**. `CONFIG_LV_COLOR_DEPTH=16` |
| 20-line strip buffer, DMA, internal RAM | `.buffer_size = 720 * 20, .double_buffer = true, .buff_dma = true, .buff_spiram = false`, `display_manager.cpp` |
| Header 64 px / rule 3 px / softkeys 96 px / 6 cards / content 1280×557 | `UI_HEADER_HEIGHT`, `UI_SHIFT_RULE_HEIGHT`, `UI_HOTKEY_HEIGHT`, `UI_CONTENT_HEIGHT` in `components/ui/styles/ui_theme.h`; `NUM_SOFTKEYS = 6` in `ui_softkey.h` |
| Empty vs disabled softkey | `SoftkeyBar::setSoftkeys()` in `ui_softkey_bar.cpp` |
| Fonts | `CONFIG_LV_FONT_MONTSERRAT_*` in `sdkconfig.defaults`; mono tables in `components/ui/fonts/` (regenerate with `scripts/gen_ui_fonts.sh`); role mapping in `ui_theme.h` |
| Palette, four compile-time themes | `WX_RGB_*` in `components/ui/styles/themes/`, roles in `ui_theme.h`, selection in `components/ui/CMakeLists.txt` |
| Widget set | `CONFIG_LV_USE_*` in `firmware/esp32/sdkconfig` |
| 30 FPS cap | `vTaskDelay(pdMS_TO_TICKS(32))` in `main/ui_task.cpp` |
| Encoder + touch + keypad | `InputDispatcher`, GT911 touch, TCA8418 keypad component. Note the encoder moves focus *within a page* (`UIPage::onInput`); there is no focus concept on the softkey bar, so a softkey is reached by touch or by a panel SOFT key, never by scrolling to it |
| Pages recreated on entry | `ui-architecture.md` "Known Limitations" #2 |
| Browse pagination, 20/page | `file_browser.cpp` (`entries_per_page = 20`) |

## Avoiding unnecessary redraws

LVGL local-style setters invalidate widgets even when the value is unchanged.
Cache visible state before calling them from recurring page services. Keep
callback/action replacement independent of this visual cache. Dense grids use
explicit flat styles without inherited blurred shadows or transitions; remove
inherited styles before setting geometry because coordinates are style-backed.
The [UI responsiveness measurements](ui-latency-notes.md) quantify this cost.

## Known discrepancy (do not design around it — it should be fixed)

The vendored BSP's `bsp_touch_new()` (`esp32_p4_nano.c`, in
`managed_components/waveshare__esp32_p4_nano/`) configures the GT911 touch
controller with `x_max = 720, y_max = 1280` — the panel's **native**
orientation — while LVGL draws to the software-rotated 1280×720 landscape
canvas (`LV_DISPLAY_ROTATION_90` in `display_manager.cpp`). Touch and
display disagree about which axis is which. If touch positions feel offset,
swapped, or compressed, this is the first suspect; verify by tapping the
four corners. Tracked in `docs/roadmap.md`.

## Iterating with Claude Design

1. Paste the brief above, plus a screenshot or description of the current
   screen, plus what is wrong with it.
2. Ask for the design as a **wireframe with px dimensions and widget names
   from the list above** — not a bitmap mockup. A design expressed as "bar
   widget, 400×24, x=40 y=180, green #4CAF50 on #333333 track" translates to
   LVGL directly; a Figma-style rendering does not.
3. Reject anything that needs per-frame animation of large areas, fonts
   outside the list, alpha compositing, or more than 6 bottom actions.
4. Ask for colours as role names (accent, card, dim, ok, warn), not hex. A
   design pinned to specific hex values silently only works in one theme.
