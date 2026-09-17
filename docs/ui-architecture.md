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
- **Project:** eight Tracks per view, Instrument assignment, MIDI input, Track level, pan/balance and mute.
- **Settings:** Display, Storage, MIDI, System, Calibrate.

  Storage offers Format Card, followed by a separate **ALL CARD DATA WILL BE
  LOST** confirmation with Cancel and Erase all data. The backend owns the
  expiring confirmation and format result; read-only polling recovers lost
  replies without replaying erasure. See [card maintenance](features/inter-mcu-protocol.md#card-maintenance).
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

Bank and Instrument Browser belong to the target
[Track/Instrument model](features/track-and-patch-model.md). A logical panel
jump key or a protocol operation does not prove the corresponding page exists.

Project replaces the Performance root label and owns the existing Track/mixer setup.
Track setup remains globally accessible; it is not nested under Sequencer.
The existing logical Track panel jump opens Project; Mixer opens its own strip view. Further
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

## Mixer page

Mixer is available from the main menu and the logical Mixer jump key. It shows
eight Track strips plus master, with a page switch for the other eight Tracks.
Track selection shares Project/Instrument/Sequencer's current Track; selecting
master leaves that Track intact. Faders commit on release, encoder rotation
and +/- softkeys adjust level by 0.5 dB or pan by 1%, and encoder click switches
level/pan focus. Mute and Solo have separate buttons. Master affects the final
stereo output; its stereo meters remain in the header.

Visible strips use one outstanding correlated request at a time, at most one
new read per 50 ms. Edits prioritize readback and block another change to that
strip until its reply; timed-out, stale and disconnected state cannot authorize
edits. Widget setters run on the UI timer and skip unchanged values. Page exit
deletes that timer and every widget. Solo has one UI-domain selection shared
with Sequencer, survives navigation, and never replaces stored manual mutes.
Per-Track peak bars consume whole cached snapshots at the UI timer cadence.
The page renews a meter lease every second and unsubscribes on exit; stale
data clears, and unchanged levels do not redraw. Rendering/touch and callback timing need
[HV-006](hardware-validation.md#hv-006--mixer-controls-and-master).

<a id="performance-page"></a>

## Project page

Project (formerly Performance/Track) selects eight Tracks per view and reads the current binding and MIDI
input from the Daisy. A selection change invalidates old readback; request
ids reject late replies, and controls remain unavailable until current data
arrives. Omni and Off are explicit choices alongside MIDI channels 1-16.
Internal pads and sequencer steps continue to address their Track directly.
The focused two-board HIL verifies routing, external setting refresh and
preservation of another Track's held note. Physical panel operation remains
a separate roadmap gate.

The selected Track also exposes Track level, pan/balance and mute through the existing
mixer operations. Correlated mixer readback reports the Daisy foreground's
accepted targets; its existing handoff applies them at the next audio block.
Mix settings remain independent of Instrument trim and replacement.
An edit retains the last confirmed display while blocking another edit until
readback. Track switches and link loss invalidate the old snapshot.
Tap or drag a control to focus it; encoder click cycles MIDI, level, pan/balance and mute,
with encoder rotation and the minus/plus softkeys adjusting the focus.
Assign opens Instrument Browser with the existing target/replacement flow;
Edit sound opens the selected Track's Instrument editor. Scene recall is
still Phase 5; Project device save/load remains a separate persistence task.
This change is compile/host verified; panel rendering and audio checks remain
open in the roadmap.

### Instrument Browser (as built, 2026-09-11)

Project → Assign and Instrument → Shift → Browse open the dedicated Instrument
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
control, MODEL, selects which filter renders that mode - SVF or Ladder (the
zero-delay-feedback ladder; see instrument-model.md) - and travels
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


### Sequencer Solo

Shift ▸ Solo on the Sequencer page solos the selected row's Track: the page
sends one `MIX_OP_SET_SOLO_MASK` selecting the audible Track, so the
engine never passes through a wrong intermediate mute set. The soloed row's
Track button turns green and its steps take a green outline; every other row
dims to half, as a row-muted row does, and the label reads `/ SOLO` or
`/ MUTE`. Shift ▸ Unsolo (or Solo on another row) sends the new mask; un-solo
clears it. Solo is frontend-owned and sticky - leaving the page does not
clear it - and the UI re-sends its mask whenever the backend link comes up,
so a reboot of either board cannot leave the engine muted behind a page that
shows nothing. It is separate from the row Mute, which stops a row triggering
and is saved with the pattern. It replaced the Step off softkey, which only
duplicated tapping the step. Console: `seqsolo` (display Track number, 0 =
none) and `SOLO <0-16>`.

### Sequencer Locks

Shift → Locks switches the existing grid's detail controls to four lock slots.
Slot/Parameter/Value drags and per-slot clearing use the same backend readback
and preview lifetime as note/velocity edits. Tapping the grid in this mode
selects a step without changing its trigger state. Grid restores normal editing.
An asterisk marks steps with locks. The focused two-board test covers four-slot
editing, individual clearing, navigation and pattern save/load.

### Stereo Mono setting

Instrument > Osc exposes Mono (Off/On) beside Keytrack. Off preserves native
stereo; On downmixes stereo. This setting uses authoritative oscillator
readback and the existing Apply/Revert baseline, but affects new notes only.
Console `MONO 0|1` and `oscmono` use the same path. Project pan is labelled
Pan / Balance: mono pans, stereo balances. The Project `MUTE 0|1` command and
`mixmute` readback use the same control as its Track Mute tile.

Project mute readback is the user's stored mute target, independent of Solo.
The backend combines that target with the temporary Solo mask in one handoff;
manual mute has priority, and clearing Solo retains edits made while soloed.

### Project files

The Project page's **Project files** button opens named Save copy/Load/New.
Load and New require explicit confirmation of unsaved-state replacement.
The page polls correlated retained backend status, disables actions on stale
readback and keeps long jobs pending without replaying mutations. Successful
recall clears transient Solo through an atomic UI-domain reset intent and
invalidates sample metadata caches; neither comm callbacks nor storage jobs
call LVGL. See [Project persistence](features/project-persistence.md) and
[HV-007](hardware-validation.md#hv-007--project-save-load-and-recovery).


### Project Pattern slots

Sequencer → Shift → Patterns opens 128 stable Project slots. Previous/Next and
the encoder choose a destination; Create and Copy active require an empty slot,
while Rename and Launch require an occupied one. Create/Copy/Rename require
stopped, unarmed playback. Launch selects while stopped or queues at the next
loop while playing; Stop cancels the queue. Launch retains outgoing edits without changing Instruments,
mixer or tempo. Files opens standalone import/export for the active slot.
The page uses correlated status snapshots and retained completion, disables
stale actions, never replays a mutation on reconnect, and deletes its timer on
exit. Project Save copy persists the complete collection. See
[Pattern management](features/pattern-management.md).

The Sequencer grid uses Pattern-scoped pages and edits. Each whole-Pattern
replacement advances an epoch; an incoming epoch clears other cached rows, and
edits echo the displayed epoch/slot so a delayed command cannot alter a newly
selected Pattern. The heading identifies the displayed slot. Grid edits remain
available while a launch is queued and are captured in the outgoing Pattern.

### Song arrangement

`UISongPage`, reached through Project Patterns' shifted Songs key, follows the
same correlated polling and retained-completion pattern as the slot/file pages.
It displays six arrangement rows, a separate section/Pattern/repeat/tempo edit
column, and the actual playing section. Draft edits require Apply/Revert; playback
freezes arrangement edits. Widgets compare values before repainting on each poll.
See [Song sequencing](features/song-sequencing.md) for operation and timing rules.
