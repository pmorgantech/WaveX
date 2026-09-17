---
name: lvgl9
description: Design, implement, or review LVGL 9.5 UI code for the WaveX ESP32-P4 frontend — pages, widgets, styling, navigation, threading, and rendering performance.
---

# LVGL 9.5 / WaveX UI skill

Use this skill for any code under `firmware/esp32/components/ui/` — pages,
navigation, styling, custom widgets, or anything that calls into LVGL 9.5.

Before making design decisions, edits, or reviews, read the complete
[`docs/ui-architecture.md`](../../docs/ui-architecture.md) (how the UI code is
structured, page lifecycle, threading) and
[`docs/ui-design-constraints.md`](../../docs/ui-design-constraints.md) (the
hardware/rendering budget and palette every design must fit). Also load the
[`esp32p4`](../esp32p4/SKILL.md) skill for anything touching FreeRTOS
tasks, DMA, or cache — the display pipeline lives on top of all of it. These
two UI docs are the detailed source of truth; do not duplicate or casually
override them.

## The concrete facts worth holding in your head

- **LVGL 9.5.0**, ESP-IDF 5.5, RGB888 (`CONFIG_LV_COLOR_DEPTH=24`), pinned via
  `main/idf_component.yml` (`>=9.4,<10`) and resolved in
  `firmware/esp32/dependencies.lock`.
- 8-DSI-TOUCH-A, JD9365 800×1280 MIPI-DSI panel, PPA-rotated 90° to 1280×800 landscape
  (`lv_display_set_rotation(display_, LV_DISPLAY_ROTATION_90)` +
  `.flags.sw_rotate = true`, `display_manager.cpp`). The port uses two partial PSRAM draw buffers (`buffer_size = BSP_LCD_H_RES * 20`)
  and PPA scratch. Two RGB888 DPI buffers are allocated, but the current partial
  flush path does not page-flip or guarantee tear-free output. The BSP disables
  rotation when tear avoidance is enabled; do not combine those modes.
- Fixed chrome: 64 px header + 3 px rule + 96 px, 6-button softkey bar → **1280×637 px**
  usable content area. UI task targets 30 FPS (`vTaskDelay(32ms)` in
  `main/ui_task.cpp`).
- Montserrat prose and JetBrains Mono numeric roles; use the compiled sizes
  listed in `ui-design-constraints.md`. Dark theme, named palette — both in
  `components/ui/styles/ui_theme.h`.
- Enabled widgets (`sdkconfig`): label, button, button-matrix, bar, slider,
  arc, chart, table, list, roller, dropdown, checkbox, switch, spinner,
  canvas, tabview. Nothing else is compiled in — don't design around a widget
  that isn't in this list without adding it deliberately (menuconfig +
  sdkconfig.defaults + a measured reason).
- The panel has GT9271 touch through `esp_lcd_touch_gt911`. Native 800×1280
  points are rotated by LVGL; corner-tap verification remains open. The driver
  rejects reports above five contacts despite the hardware's ten-touch rating.
  Backlight is I2C through the BSP; verify brightness/blank-wake on the panel.
  See `ui-design-constraints.md` for the wiki/BSP register discrepancy.


## Page contract (`UIPage`, `ui/ui_page.h`)

Every screen is a `UIPage` subclass owned by `UINavigator`'s push/pop stack —
never a global pointer. Implement `name()` and `onEnter(lv_obj_t* parent)`;
override `onExit()`, `onInput()`, `onTrackChanged()`, `getSoftkeys()`,
`getShiftedSoftkeys()`, `consoleState()`/`consoleCommand()` only as needed —
all have empty/no-op defaults, so a page opts in to exactly what it uses.

- **Widgets are recreated on every `onEnter`**, deleted on `onExit`/pop. Don't
  design a page that depends on preserved off-screen widget state.
- Softkeys are a fixed 6-slot row (`NUM_SOFTKEYS`, `ui_softkey.h`). A disabled
  key renders dimmed with a `why` string, not hidden — the row never
  reflows. Softkey 1 is conventionally "Back".
- `getShiftedSoftkeys()` is the Shift-revealed alternate row; a page that
  doesn't define one is simply inert while shifted (`UINavigator` checks
  `activePageHasShiftedKeys()` before treating Shift as meaningful). Shift
  itself is intercepted globally by `InputDispatcher`, never per-page.
- Two distinct grouping shapes exist — pick per
  [`ui-architecture.md`](../../docs/ui-architecture.md)
  under "Navigation structure":
  - `UITabHostPage` hosts independent existing `UIPage`s unchanged (Sample,
    Settings) — lazy entry, exits a hidden tab so it holds no LVGL objects.
  - `tabGroupCreate()`/`tabGroupAddTab()` (`ui_tab_group.h`) build one shared
    `lv_tabview` for stages that must share state across a tab switch
    (`UIInstrumentPage`'s five stages, Diagnostics's seven tabs). Reuse this
    helper's styling rather than copying it inline — that's precisely what it
    was extracted to stop.
- New pages: add a factory function and register it in `ui_main_menu.cpp`
  (`ui_navigation_integration.cpp` only bootstraps the root, it doesn't
  register individual pages). Worked examples in rough order of complexity:
  `ui_main_menu.cpp` (registration), `ui_play_page.cpp` (paged softkey params,
  softkey-refresh-on-state-change), `ui_sample_browser.cpp` (state
  preservation, paginated backend data with a loading row), and
  `ui_diagnostics_page.cpp` (tab group driven by pushed telemetry).

## Threading — the one rule that must never be broken

LVGL's core is not thread-safe; this project's lock is `esp_lvgl_port`'s
`lvgl_port_lock()`/`lvgl_port_unlock()`, wrapped as a local `LV_LOCK()`/
`LV_UNLOCK()` macro pair **redefined at the top of every `.cpp` file that
needs it** (`display_manager.cpp`, `ui_navigator.cpp`, `ui_menu_page.cpp`,
`ui_softkey_bar.cpp`, `ui_settings_page.cpp`, `main/ui_task.cpp`) — follow
that same copy-paste convention rather than inventing a shared header or
calling generic `lv_lock()`/`lv_unlock()` directly.

- `onEnter()`/`onExit()` and anything called from the UI task's normal loop
  already run with the lock held. The port lock is recursive; avoid redundant
  nesting and keep existing lock pairs balanced.
- **Never call an LVGL function from a background task** (UART RX task, meter
  timer, console task). The pattern instead: background task publishes a complete value
  through a synchronized mailbox or queue; a UI service point consumes it,
  releases the snapshot lock, then applies it under the LVGL port lock.
  A volatile struct or pending flag alone does not protect its fields.
  `BusyOverlay::requestProgress()`/`requestHide()` + `service()` and the
  meter handoff illustrate the service points — read
  `ui_busy_overlay.h` and the "Cross-task updates" section of
  `ui-architecture.md` before adding a new cross-task update path.
- `lv_async_call()` itself requires LVGL synchronization; it is not safe
  to invoke from UART reception without that lock. Queue values to a UI
  service point instead, preserving the LVGL → UART lock order.
- The LVGL/UI task stack was raised to 16 KB deliberately
  (`display_manager.cpp`, `initLvglDisplay()`) after a real stack-overflow
  panic from heavy `onEnter()` object counts plus `lv_label_set_text_fmt`'s
  `vsnprintf`. That headroom is not a license to be careless with stack in
  page code — a page building dozens of widgets in one `onEnter` is normal
  here, but check the logged high-water mark rather than assuming.

## Styling

Use the constants in `styles/ui_theme.h` — `UI_COLOR_*`, `UI_FONT_*`,
`UI_HEADER_HEIGHT`/`UI_HOTKEY_HEIGHT`/`UI_PADDING_*` — never literal colors,
font pointers, or pixel constants in page code. A palette or font change must
land in one file. Use `ui_theme_apply_button_style()` /
`ui_theme_apply_container_style()` / `ui_theme_apply_label_style()` for the
common cases rather than hand-building style objects per page.

LVGL 9.5 adds native drop shadow and blur (software, no GPU). **Treat both as
expensive by default on this target**: a drop-shadow experiment already
hung the UI outright by requesting a layer the LVGL memory pool
(`CONFIG_LV_MEM_SIZE_KILOBYTES=128`) couldn't satisfy (`docs/roadmap.md` §0.3
item 2, and the memory overlay in `performance_monitoring.md` exists partly
because of this incident). Don't add shadow/blur styling without checking the
memory monitor overlay and the FPS/CPU sysmon numbers before and after, on
real hardware.

## Custom widgets: prefer composition, justify canvas drawing with a measurement

The enabled widget list above covers most needs — compose standard widgets
before writing a custom `LV_EVENT_DRAW_MAIN` handler. When you do need one,
`components/waveform_view.cpp`/`.h` is the worked example and the cautionary
tale: it used to be an `lv_chart` and cost 37–44 ms on first frame because
anti-aliased line segments are the wrong tool for a solid waveform silhouette.
It now draws opaque rectangular spans directly onto the layer — no
anti-aliasing, no per-point widget state, into fixed `std::array` columns
allocated once at construction (no allocation after construction, per
`esp32p4_coding_guide.md` §8). If you write a new canvas-drawn widget, hold it
to the same bar: measure the widget you're replacing (or an `lv_chart`
prototype) with log-mode sysmon before deciding a hand-drawn version is
justified, and keep its buffers pre-allocated and fixed-size.

## Build pages with bounded repainting

Apply this while designing and implementing every new or changed page, before
it becomes a later optimization task. See
[UI latency measurements](../../docs/ui-latency-notes.md) for the measured
Sequencer, Play, Instrument and Diagnostics examples and profiling commands.

- **Define the visual update model first.** Identify backend-owned values,
  transient input state and derived presentation. A poll/reply or new request
  ID alone must not redraw unchanged content. Retain the last confirmed
  snapshot during an in-flight refresh, but keep edit readiness separate so
  stale data cannot authorize a second mutation.
- **Make repeated refreshes idempotent.** Reuse the value-tile, dial and chrome
  helpers. Compare displayed text, font, focus/tone styles, table cells and
  curve points before calling setters that invalidate on identical input.
  Check the pinned LVGL implementation: some setters already deduplicate,
  while label/table/style setters can still allocate or invalidate. Prefer
  widget-owned values to a second model, especially for copied widget handles.
- **Cache only what widget getters cannot represent reliably.** A pressed
  style can override the base colour: Play caches its applied base colour so
  latch highlighting survives release. Reset visual caches when widgets are
  recreated. Keep callback/command metadata current even when appearance does
  not change; skipping a redraw must never retain a stale action.
- **Keep one interaction local.** Restyle the changed key/cell and previous
  selection, not every row or sibling. Rewriting enough small objects can
  overflow LVGL invalidation capacity and become a full-screen repaint. Set
  static styles/geometry at creation; avoid inherited shadows/transitions on
  dense grids. Use supported style removal, never a null transition descriptor.
- **Preserve meaningful live updates.** Changed values, warning/offline states
  and time-series history must still update. A new sparkline sample can be
  meaningful even when its value equals the previous sample. Avoid reducing
  telemetry cadence or freezing the view merely to conceal repaint work.
- **Check rendering before page completion.** Exercise idle polling, repeated
  identical replies, one control change, selection/latch/press/release, and
  exit/re-entry. Use a real-LVGL host test for reusable update helpers where
  useful. On hardware, separate page-entry cost from steady interaction;
  record submitted pixels/full-screen refreshes and refresh mean/peak with
  profiling-only `RENDER` and log-mode sysmon. A local edit should not require
  a full-screen refresh unless its actual layout/content change justifies it.
  Compare under the same workload, check appearance and note/control lifetime,
  and leave profiling disabled in the final image. If hardware is unavailable,
  report that gate open rather than asserting responsiveness.

`RENDER` measures pixel-submitting refresh wall time; sysmon busy percentage
is LVGL activity, not whole-ESP32 CPU utilization. Neither establishes physical
touch-to-visible-panel or audio latency. Use the dedicated input/readback trace
when attributing interaction delay rather than blaming the transport.

## Measuring instead of guessing

Never claim a WaveX UI change is faster or "smoother" without a number — see
[`docs/performance_monitoring.md`](../../docs/performance_monitoring.md) Part
2 in full before touching rendering-sensitive code. In brief:

- Enable `CONFIG_LV_USE_SYSMON` + `CONFIG_LV_USE_PERF_MONITOR` +
  `CONFIG_LV_USE_MEM_MONITOR` for a measurement build only, never in a
  shipped one.
- For anything you'll act on, use **log mode**
  (`CONFIG_LV_USE_PERF_MONITOR_LOG_MODE=y`) over the on-screen overlay — it
  gives `render`/`flush` split per line, which the overlay doesn't. `render`
  is what a draw-unit change (e.g. the PPA) can help; `flush` is pixels-to-
  panel and nothing in LVGL touches it. A page dominated by `flush` will not
  be helped by rendering optimization.
- `scripts/serial_log.py` + `scripts/sysmon_stats.py` capture and compare runs
  (mean/median/p95/IQR) — one page per log file, FPS is not comparable across
  pages, and check the IQRs overlap before calling a delta real.
- `CONFIG_LV_USE_REFR_DEBUG=y` (tints redrawn regions) is the first thing to
  check on a slow page — coarse invalidation causing full-width redraw for a
  one-label change is a layout bug, not a rendering one, and is more common
  here than an actually-slow draw unit.
- If `CONFIG_LV_USE_PPA` is in play: it accelerates opaque rectangle fills but
  also replaces LVGL's cache-invalidation callback with a whole-buffer
  `esp_cache_msync` twice per draw task (`display_manager.cpp`'s
  `wavexInvalidateCacheArea` narrows this to the dirty area — read that
  comment before touching PPA/cache code). A PPA A/B test that shows PPA
  losing should suspect the cache callback before the hardware.

## Screenshot tooling

`ui_screenshot.h`/`.cpp` + `scripts/esp32_screenshot.py` capture the active
LVGL screen via `lv_snapshot` under the lock, RLE+base64 it over the debug
console UART, and decode to PNG host-side — debug builds only
(`WAVEX_ESP_SCREENSHOT_DEBUG`). Use this to visually verify a page change
without a physical panel in reach; it captures under `LV_LOCK()` from
`wavex_screenshot_poll()`, called once per UI-task loop pass, so it never
blocks the render loop for more than one frame.

## Review gate

For every significant LVGL/UI review:

- Apply the decision filter in `docs/project-principles.md`; cite the
  relevant principle number(s) on constitutional findings (esp. Principle 3
  hardware independence, 5 explicit ownership, 6 immutable snapshots across
  task boundaries, 14 favor simplicity).
- Trace every path that touches an LVGL call: is it inside `onEnter`/`onExit`,
  under an explicit `LV_LOCK()`/`LV_UNLOCK()` pair, or reached from a
  background task? The last one is a bug regardless of how unlikely the race
  looks.
- Check the 1280×637 content budget, the fixed 6-softkey contract (dimmed not
  hidden, `why` set when disabled), and that styling uses `ui_theme.h`
  constants rather than literals.
- Check that a new custom-drawn widget is justified by a measurement against
  the standard-widget alternative it replaces, not just aesthetic preference.
- Check the bounded-repainting workflow above for new pages and repeated
  refresh paths, not only when the user reports lag.
- Check that a performance claim (faster page, PPA on/off, shadow/blur added)
  cites log-mode sysmon numbers with render/flush split, not the on-screen
  overlay or an impression.
- Check roadmap phase alignment (`docs/roadmap.md`) and that
  `docs/ui-architecture.md`/`ui-design-constraints.md` are updated in the same
  change when the page contract, chrome dimensions, palette, or navigation
  shape intentionally changes. Record unrelated UI debt in `docs/roadmap.md`
  rather than expanding the reviewed patch — several open items already exist
  there (touch axis mismatch, busy-overlay error path, softkey allocation).

Only report concrete, actionable findings; the principles are not a reason to
manufacture style objections or demand a big-bang refactor.
