# WaveX UI Architecture and Navigation

**Status:** As-built framework reference, reviewed 2026-09-13. This document
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
| BSP + `esp_lvgl_port` | Own the GT911 controller, LVGL tick and rendering task; do not initialize a second touch driver or tick |
| `MultiTouchInput` + `multi_touch_port.cpp` | Adapt the BSP touch registration to five independent pointers, sampled together under the LVGL port lock |
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
[roadmap.md](roadmap.md). Do not present them as an implemented abstraction.

## Navigation structure

- **Sample:** Browse, Edit, Manage, Record.
- **Play:** Pads and Keys, sharing note lifecycle and live parameters.
- **Instrument:** Sample, Env, Amp, Filter, Mod, LFO.
- **Performance:** eight Tracks per view, Instrument assignment, MIDI input, Track level and pan.
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

Bank, Instrument Browser and Mixer belong to the target
[Track/Instrument model](features/track-and-patch-model.md). A logical panel
jump key or a protocol operation does not prove the corresponding page exists.

Performance replaces the Track root label and extends the existing page.
Track setup remains globally accessible; it is not nested under Sequencer.
The existing logical Track panel jump opens Performance. Further Mixer,
Bank, Scene and Song work is tracked in the
[roadmap](roadmap.md#composition-and-performance-workflows).

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

The GT911 adapter reads one complete snapshot per LVGL refresh period and
routes up to five contacts to separate pointer devices by tracking ID, not
array index. Releases are delivered before a slot is reused. Each pointer
uses LVGL's existing display rotation, and any finger can wake the screen.
This supports simultaneous interactions with separate controls; pinch/rotate
gestures are not enabled. I2C errors release all contacts. The integration
wraps the public `lvgl_port_add_touch` / `lvgl_port_remove_touch` entry points,
leaving the BSP's controller initialization and managed sources intact.
The adapter owns its sampling timer and pointers; removal stops the timer
before deleting them. Physical two-finger behavior still needs the bench
checks in [roadmap.md](roadmap.md#outstanding-hardware-verification).

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

Shift toggles on touch-down so a second finger can press a shifted action
while the first remains on Shift. A tap still latches it: the header chip and rule indicate it, a shifted action
consumes it, and navigation clears it. Pages may supply
`getShiftedSoftkeys()`; use the navigator's shifted-key query rather than
implementing a second modifier policy.

Softkey callbacks wait in an eight-entry queue owned by the bar. Navigation
and tab changes cancel queued callbacks before exiting their page; an action
already running is removed from the queue before it can navigate. Queue-full
or LVGL scheduling failure rejects the press without running it inline.

When action labels or enabled states change, update the page model and call
`UINavigator::refreshSoftkeys()`. The bar replaces actions and metadata on
every refresh, but changes LVGL labels/styles only when their appearance
changes. The Shift chip/rule use the same visual-state caching policy.
Cache lifetime ends when widgets are recreated; identical labels do not
mean identical callbacks.

Sequencer cells toggle once on touch-down. Their UI model retains the last
confirmed row while readback is pending; that retained picture does not
make the row editable. A matching validated Daisy reply authorizes further
toggles. Window/link invalidation discards the picture. Page servicing runs
every 16 ms while background row requests retain their existing cadence.
See [UI latency measurements](ui-latency-notes.md) for hardware evidence.

Shared value-tile and dial setters compare their current LVGL properties
before invalidating widgets. Widget properties remain the visual source of
truth because page-owned handles can be copied. Play keys separately cache
applied base colours: the temporary pressed-state colour is not the latched
base colour. Instrument curves keep page-owned point arrays and only reset
LVGL's points when geometry changes or a new line widget needs its array.

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
3. Register a factory in `ui_navigation_integration.cpp`, then expose it as a root item
   or tab child through the existing navigation model.
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

Diagnostics keeps its existing live sample and sparkline cadence, while card
text, warning colours and table cells update only when their displayed values
change. The shared header similarly avoids resetting identical title/context
text; geometry is recomputed when either string changes.

## Verification and remaining work

Use the real-LVGL leaf-widget tests described in
[testing_guide.md](testing_guide.md#lvgl-widget-tests). Page navigation,
listener teardown under traffic, real touch coordinates and panel memory
headroom still need the relevant host boundary tests or HIL/bench checks.

Open UI ownership work stays in [roadmap.md](roadmap.md); panel acceptance
and rendering/audio measurements stay in
[roadmap.md](roadmap.md#outstanding-hardware-verification) and
[performance_monitoring.md](performance_monitoring.md).
Do not copy those task lists into this reference.

## Related

- [UI design constraints](ui-design-constraints.md)
- [ESP32-P4 coding guide](esp32p4_coding_guide.md)
- [System architecture](architecture.md)
- [Track/Instrument model](features/track-and-patch-model.md)


## Touch sequencing and kits

Sequencer is a main-menu root page with a paged four-Track/sixteen-step grid.
Instrument > Sample > Pad Map opens the selected Track's kit editor, with
resident-sample assignment, audition, choke, a name keyboard and new-copy
saves. Both pages consume synchronized backend snapshots on UI timers.
Their touch workflows do not require physical panel wiring.

## Performance page

Performance (formerly Track) selects eight Tracks per view and reads the current binding and MIDI
input from the Daisy. A selection change invalidates old readback; request
ids reject late replies, and controls remain unavailable until current data
arrives. Omni and Off are explicit choices alongside MIDI channels 1-16.
Internal pads and sequencer steps continue to address their Track directly.
The focused two-board HIL verifies routing, external setting refresh and
preservation of another Track's held note. Physical panel operation remains
a separate roadmap gate.

The selected Track also exposes Track level and pan through the existing
mixer operations. Correlated mixer readback reports the Daisy foreground's
accepted targets; its existing handoff applies them at the next audio block.
Mix settings remain independent of Instrument trim and replacement.
An edit retains the last confirmed display while blocking another edit until
readback. Track switches and link loss invalidate the old snapshot.
Tap or drag a control to focus it; encoder click cycles MIDI, level and pan,
with encoder rotation and the minus/plus softkeys adjusting the focus.
Assign opens Instrument Browser with the existing target/replacement flow;
Edit sound opens the selected Track's Instrument editor. Scene recall is
still Phase 5, and this page does not save the Performance to a Project.
This change is compile/host verified; panel rendering and audio checks remain
open in the roadmap.

### Instrument Browser (as built, 2026-09-11)

Performance → Assign and Instrument → Shift → Browse open the dedicated Instrument
Browser. It reuses the existing browser lifecycle with independent directory and
selection state. The Daisy filters WXI/SFZ before pagination; Sample → Browse
lists WAV files. Saved opens /wavex/instruments, Root opens /. Selecting an
Instrument inspects its referenced samples; Load asks for the target Track and
confirmation before replacement. The kit HIL test recalls its saved WXI through
this flow and checks cancellation and preserved pad overrides.

### Key Map (as built, 2026-09-11)

Instrument → Shift → Key Map opens the keyboard editor. It shares naming, sample
selection and save-copy flows with Pad Map. Eight of 32 stable slots appear per
view; Shift → Previous 8 / Next 8 pages them. Assign selects a resident sample.
Five draggable controls stage key low/high, velocity low/high and root note;
Apply publishes the ranges and Revert discards them. Inclusive limits stay
ordered. A replacement revision discards a stale draft.

New keys confirms replacement before naming an empty keyboard Instrument.
Shift provides Clear zone, Rename and a short note audition. Range edits preserve
held voices; assignment/clear stops only the edited Track. Replacing a sample
resets its zone's sample-specific region/loop markers, retaining key ranges,
tuning, gain and sound settings. Save copy preserves sparse slots and ranges in
WXI; Instrument Browser recalls the result. Bank and Program Change recall remain
Phase 2.5 work. No physical panel gate is implied by these touchscreen controls.

The final editor regression selection passed 17 two-board HIL tests, covering
Key Map, Pad Map overrides, Track routing, Instrument/Sample loading, shared pool
ownership and the sequencer grid. The 1280×720 Key Map capture was inspected on
2026-09-11 after adjusting control heights to separate values from their fill
bars. The capture is local at logs/key-map-20260911.png (gitignored).

### Instrument LFO (as built, 2026-09-12)

Instrument → LFO presents two selectable per-voice LFOs. Each selection uses
eight value tiles arranged in two rows of four: the LFO selector plus waveform,
rate in Hz, sync division, retrigger, delay, fade and pitch-follow. The page
uses the shared revisioned transport and common sound Apply/Revert actions, so
edits preview automatically and a WXI save persists the audible working copy.
Navigation waits for an outstanding delivery but does not require Apply/Revert
before leaving the tab. Held LFO, oscillator, gain and Env 2/3 edits use the
bounded live handoff, preserving source cursors, loop state and envelope/LFO
phase. Map/sample assignment and Instrument replacement retain their
stop/next-note boundary.
The Filter tab's third control selects the Instrument-owned response mode:
LP, HP, BP or Notch. It replaces the former inert Env Amount placeholder and
coalesces mode, cutoff and resonance into the typed filter-settings edit;
Apply/Revert and WXI save use the same audible working-copy path. The fourth
control, MODEL, selects which filter renders that mode - SVF, Ladder, Ladder
2x or ZDF (the 2026-09-14 A/B set; see instrument-model.md) - and travels
in the same edit, so it previews on held notes, undoes with Revert and
persists with the Instrument. The console exposes it as `filtertopology` and
`MODEL <0|1>` on the Filter tab. The fifth and sixth controls, SLOPE (12 or
24 dB) and DRIVE (0-100%), complete the Instrument-owned filter: same edit,
same preview/undo/save path, `filterslope` / `filterdrive` in the console
state and `SLOPE <0|1>` / `DRIVE <0-1000>` on the tab. The Filter tab row is
six tiles wide; the three word-valued tiles use the smaller mono value font
so "Band-pass" and "Ladder 2x" fit (`logs/filter-tab-20260914.png`).
The frontend suite passes 274 tests, and the two-board LFO HIL passes the
save/readback flow (11.70 seconds). The inspected 1280×720 capture is
`logs/instrument-lfo-20260912.png`.


### Sequencer Locks

Shift → Locks switches the existing grid's detail controls to four lock slots.
Slot/Parameter/Value drags and per-slot clearing use the same backend readback
and preview lifetime as note/velocity edits. Tapping the grid in this mode
selects a step without changing its trigger state. Grid restores normal editing.
An asterisk marks steps with locks. The focused two-board test covers four-slot
editing, individual clearing, navigation and pattern save/load.
