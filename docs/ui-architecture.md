# WaveX UI Architecture and Navigation

**Status:** As-built framework reference, reviewed 2026-09-06. This document
owns navigation, page lifecycle and UI threading. Display dimensions, fonts,
palette and rendering limits live in
[ui-design-constraints.md](ui-design-constraints.md).

## Contents

- [Runtime owners](#runtime-owners)
- [Navigation structure](#navigation-structure)
- [Page contract and lifetime](#page-contract-and-lifetime)
- [Input and softkeys](#input-and-softkeys)
- [Cross-task updates](#cross-task-updates)
- [Adding a page](#adding-a-page)
- [Verification and remaining work](#verification-and-remaining-work)

## Runtime owners

| Owner | Responsibility |
|---|---|
| `DisplayManager` | Starts the BSP display, configures software rotation, exposes panel/display handles and services brightness/blanking |
| BSP + `esp_lvgl_port` | Own the GT911 input registration, LVGL tick and rendering task; do not initialize a second touch driver or tick |
| `UINavigator` | Owns the page stack and shared header/content/softkey chrome |
| `UITask` | Drains queued panel/encoder input and services deferred application updates |
| LVGL port task | Runs LVGL timers, touch events and rendering |
| UART receive task | Parses messages and publishes data; never calls page/widget APIs |

Both the application UI task and LVGL port task can execute UI code.
The shared `lvgl_port_lock()` / `lvgl_port_unlock()` serializes their
access to LVGL and UI state. Input dispatch takes this lock per event;
LVGL events and timers execute within the port's handler context. Do not
describe this as one FreeRTOS task owning every callback.

The two UI task stacks are configured independently; see the task inventory
in [architecture.md](architecture.md#43-esp32-frontend-runtime-model).
Page construction can execute through either input path, so inspect both
high-water marks on hardware.

`DisplayManager` is a singleton, as are the navigator and input dispatcher.
Pages still call `inter_mcu_*` functions in `main`; a fully injected
`UISharedContext` and removal of that dependency cycle are future work in
[backlog.md](backlog.md). Do not present them as an implemented abstraction.

## Navigation structure

- **Sample:** Browse, Edit, Manage, Record.
- **Play:** Pads and Keys, sharing note lifecycle and live parameters.
- **Instrument:** Sample, Env, Amp, Filter, Mod.
- **Settings:** Display, Storage, MIDI, System, Calibrate.
- **Diagnostics:** ESP32, Daisy, Audio, Link, Storage, MIDI, Panel.

`ui_main_menu.cpp` registers root groups and their factories.
`ui_navigation_integration.cpp` bootstraps the root and active input context.

Use `UITabHostPage` for independent existing pages, such as Sample and
Settings. It lazily enters the selected child and exits it when switching
away. A hidden child must release its timers, listeners and widget pointers.

Use `tabGroupCreate()` / `tabGroupAddTab()` for a page whose stages share
state across tab changes, such as Instrument and Diagnostics. Reuse the
shared chrome rather than duplicating styles. Diagnostics builds tab bodies
lazily to bound entry work.

Track, Bank, Instrument Browser, Mixer and expanded oscillator/envelope/LFO
editors belong to the target
[Track/Instrument model](features/track-and-patch-model.md). A logical panel
jump key or a protocol operation does not prove the corresponding page exists.

## Page contract and lifetime

`UIPage` lives in `components/ui/include/ui/ui_page.h`.
`UINavigator` keeps `shared_ptr<UIPage>` entries on its stack:

- `name()` and `onEnter(parent)` are required.
- `onExit()`, `onInput()`, `onTrackChanged()`, softkey definitions,
  context and debug-console methods are optional overrides.
- `push()` exits the current page before entering the new one.
  `pop()` exits the top and re-enters its predecessor.
- Persistent model state may remain in the page object; LVGL widgets are
  recreated on entry. Never use widget pointers retained from a prior entry.
- Tear down timers and listeners before destroying their target state.
  Clear listener registrations through their owning API so an in-flight
  callback cannot outlive the page.
- `contextLine()` identifies the Track, Instrument or sample being edited.
  The navigator copies its text; call `refreshContext()` when it changes.

The selected Track (`ui/current_track.h`) and selected sample
(`ui/current_sample.h`) are explicit shared UI state. Pages must not infer
them from the previous page. Replacing an occupied Track follows the
confirmation policy in the Track/Instrument model.

The header status strip owns its LVGL timer and reads meter snapshots from
`inter_mcu_get_meter_data()`. There is no second `UITask` meter timer.
Backend uptime comes from the heartbeat; storage diagnostics have one
authoritative source on the Storage tab.

## Input and softkeys

`InputDispatcher` has a bounded queue shared by keypad, encoder and debug
input producers. Producers post value events; the UI task drains them under
the LVGL port lock.

The global `PanelKey` mapping handles Back, Shift, six softkeys, root jumps
and Track changes before forwarding page input. Hardware driver status and
future LED/pot work live in [panel-controls.md](features/panel-controls.md).

`SoftkeyBar` always has six positions. Touch and mapped panel SOFT keys
invoke the displayed action. Encoder events are handled by the active page;
there is no implemented encoder-focus traversal of the softkey bar.
Use theme roles for text/colour and preserve empty versus disabled actions.

Shift is latched: the header chip and rule indicate it, a shifted action
consumes it, and navigation clears it. Pages may supply
`getShiftedSoftkeys()`; use the navigator's shifted-key query rather than
implementing a second modifier policy.

When action labels or enabled states change, update the page model and call
`UINavigator::refreshSoftkeys()`.

Pads and Keys must release notes on release, `PRESS_LOST`, exit and changes
that invalidate their held-note map. Latch and panic share that lifecycle.
Browser audition streams a file; loading and binding a resident sample makes
it playable through an Instrument.

## Cross-task updates

A background callback copies a complete value into a synchronized mailbox or
bounded queue. A UI service point consumes it, releases the mailbox lock,
then changes widgets in the LVGL context.

A `volatile` struct plus a pending flag is not synchronization. Even a
release/acquire flag does not protect a slot if the producer can overwrite
its fields while the consumer is copying them. Use explicit slot ownership,
a queue, or a short mutex-protected snapshot. Scalars that do not form a
joint invariant can use atomics.

Lock order is **LVGL → UART**. UART receive callbacks must not acquire the
LVGL lock. Snapshot locks must not be held while sending a packet or invoking
arbitrary callbacks, to avoid a reverse lock dependency.

`lv_async_call()` itself uses LVGL's mutable timer/allocation state; it is
not a thread-safe escape hatch for a UART callback. Queue data to an existing
UI service point instead. See the upstream
[LVGL threading contract](https://lvgl.io/docs/open/9.5/integration/overview)
and use WaveX's port lock, not a separate unrelated mutex.

The port lock is recursive in the vendored integration. Existing navigator
entry points may acquire it while a caller already holds it; keep those
pairs balanced. Avoid adding redundant nesting inside page callbacks.
Never call LVGL from an ISR or hold a UI lock across long file/link waits.

## Adding a page

1. Implement a `UIPage` in `components/ui/pages/`; place public
   declarations in `include/ui/`.
2. Create widgets in `onEnter()`; release listeners, timers and owned
   resources in `onExit()`.
3. Register a factory in `ui_main_menu.cpp`, as a root item or tab child.
   The navigator owns the resulting page; do not add a global page pointer.
4. Bind labels, dimensions and fonts through `styles/ui_theme.h`.
   The constraints guide owns their concrete values.
5. Route remote data through a complete synchronized handoff and refresh
   softkeys/context from the same UI model used to render the page.

Use current pages as examples: `ui_play_page.cpp` for notes and live
parameters, `ui_sample_browser.cpp` for request-driven browsing,
`ui_diagnostics_page.cpp` for subscription lifetime.
The Sample tabs share `EnvelopePanel`, which owns waveform requests,
caching and retry policy; do not start a second preview pipeline. Window
changes draw cached data on the next UI service, while only missing-data
requests wait for the 150 ms settle. Complete finer-tier runs also satisfy
coarser views without a transfer. The router expands the 8-bit wire extrema
onto the renderer's signed 16-bit scale. The renderer emits clipped, merged fills; a full waveform bitmap is not
retained.
Transfer scheduling is documented in
[inter-mcu-protocol.md](features/inter-mcu-protocol.md#waveform-transfer-scheduling-as-built).

## Verification and remaining work

Use the real-LVGL leaf-widget tests described in
[testing_guide.md](testing_guide.md#lvgl-widget-tests). Page navigation,
listener teardown under traffic, real touch coordinates and panel memory
headroom still need the relevant host boundary tests or HIL/bench checks.

Open UI ownership work stays in [backlog.md](backlog.md); panel acceptance
and rendering/audio measurements stay in
[roadmap.md](roadmap.md#outstanding-hardware-verification) and
[performance_monitoring.md](performance_monitoring.md).
Do not copy those task lists into this reference.

## Related

- [UI design constraints](ui-design-constraints.md)
- [ESP32-P4 coding guide](esp32p4_coding_guide.md)
- [System architecture](architecture.md)
- [Track/Instrument model](features/track-and-patch-model.md)
