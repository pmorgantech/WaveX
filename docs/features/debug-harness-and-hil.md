# Debug Harness & Hardware-in-the-Loop Testing

**Status**: **Built 2026-09-04** — §8 stages 1–6 landed in one pass and
`make test-hil` runs 19 tests against both boards (the harness itself, page
routing by injected input, and the Track/Instrument model's stage-2 Load-to-Track
workflow end to end). §10 records what was built as against what was proposed,
and what the first bench run found. The console channel it extends is the
model for everything here — see [`logging.md`](../logging.md).

**Where it fits**: this is test infrastructure, not a Phase 2 feature. It
belongs beside [`testing-remediation.md`](../testing-remediation.md) rather than
in a roadmap phase, and it earns its place now because the roadmap's
§ Outstanding hardware verification table has grown to ~20 rows of *manual bench
procedures*, several of which say in as many words that the fix was made by
reading code and nobody has seen it work. §7 below maps specific rows of that
table onto specific automated tests. A harness that turns a third of that table
into `make test-hil` is worth more than any one of the features waiting behind
it.

---

## 1. What exists today

Three ASCII tokens are already accepted on the boards' debug serial links, and
they are the whole of the remote-control surface:

| Token | Board | Where | Compiled out in release? |
|---|---|---|---|
| `WAVEX-ENTER-DFU` | Daisy | `daisy/src/main.cpp:64,96-104` | No, deliberately so ([Build profiles](../../README.md#build-profiles)) |
| `WAVEX-LOG <...>` | Daisy | `daisy/src/main.cpp:77-128, 512-526` | Yes, through `WAVEX_DEBUG_HARNESS_ENABLED` |
| `WAVEX-LOG <...>` | ESP32 | `esp32/components/ui/src/ui_screenshot.cpp:145-202` | Yes, `WAVEX_ESP_SCREENSHOT_DEBUG` |
| `WAVEX-SCREENSHOT` | ESP32 | `esp32/components/ui/src/ui_screenshot.cpp:204-278` | Yes, same flag |

The grammar parser is shared and host-tested (`ApplyLevelCommand`,
`shared/config/logging_config.h:230`; `shared/tests/config/logging_config_test.cpp`),
which is the property to preserve: **the command grammar cannot drift between
boards because neither board owns it.**

Three things are missing, and they are what this document adds:

1. **No input injection of any kind.** Nothing anywhere fabricates a button
   press, a detent or a tap.
2. **No machine-readable state.** A host can read the log, which is prose.
3. **No acknowledgement.** A command is written to the port and the sender
   hopes. The Daisy will silently *drop* a `WAVEX-LOG` that arrives while a
   previous one is still pending (`main.cpp:118-123`) — invisible to a human
   typing one command, fatal to a script issuing fifty.

### The touch finding

`InputType` declares `TouchDown`, `TouchUp` and `TouchMove`
(`ui/input_event.h:9-11`) and **nothing constructs any of them.** Touch does not
pass through `InputDispatcher` at all: GT911 → `esp_lvgl_port` → LVGL widget
callbacks, directly. The only real producers of `InputEvent` are the TCA8418
keypad (`tca8418_keypad.cpp:55`) and the encoder/pot poll in the UI task
(`ui_task.cpp:221,255`).

This is why "fire a button push or a screen tap" is not one mechanism. Buttons
and detents inject into a queue; taps have to be injected into LVGL itself.

---

## 2. Injection points

**The principle, and it is the whole reason this is worth building: inject at
the boundary where the real driver hands off to the application, and never one
layer above it.** A harness verb that called `UINavigator::push(SampleEditPage)`
would make a test that proves a page renders and proves nothing about whether
you can *get* to that page. Injecting a keypress that the navigator routes for
itself tests the routing, the page, and everything between. The corollary is
§6's honest limitation: nothing below the injection point is tested either.

| What | Injection point | Context |
|---|---|---|
| Button press/release | `InputDispatcher::instance().post()` (`input_dispatcher.h:19`) | Harness console task |
| Encoder / pot detents | Same, `EncoderLeft/Right/Up/Down` | Harness console task |
| Touch down/move/up | A second LVGL `indev` (below) | lvgl_port task |
| Daisy backend commands | `WaveX::Comm::ProcessInterMcuMessage()` (`daisy_inter_mcu_message_handlers.h:10`) | Daisy main loop |

`InputDispatcher::post()` is already a public, multi-producer entry point with a
bounded queue and a drop counter, so button and encoder injection needs no new
plumbing whatsoever — the harness becomes a third producer alongside the keypad
task and the UI task.

`ProcessInterMcuMessage` is the transport-agnostic dispatcher every Daisy
message already flows through, whichever link delivered it. Injecting there
gives the host complete control of the backend **without the ESP32 in the
loop** — which is what makes a Daisy-only regression test possible, and what
makes malformed-packet fault injection possible.

### Synthetic touch: a second indev

Register an additional `LV_INDEV_TYPE_POINTER` indev whose `read_cb` drains a
small FreeRTOS queue filled by the harness console task. LVGL supports multiple
indevs of one type; the real GT911 indev is not modified, wrapped or displaced,
so a release build is byte-identical to today's and a debug build has one extra
indev that reports "not pressed" unless a test is driving it.

Two details that decide whether this works at all:

- **A tap is not one event.** LVGL derives click/long-press/gesture from press
  state observed across successive `read_cb` calls, so `TAP x y` must hold
  `LV_INDEV_STATE_PRESSED` for a bounded number of read cycles and then release.
  A single-cycle blip is a different gesture from a tap and some widgets will
  not see it.
- **`TOUCH DOWN/MOVE/UP` must be separately addressable**, not just `TAP`.
  Roadmap 1.5.2 item 2 (draggable marker handles) is unbuildable-by-test
  without drag injection, and it is the next UI work that needs it.

`read_cb` runs in lvgl_port task context (never an ISR), so an ordinary
`xQueueReceive` with a zero timeout is the right primitive — a real
synchronisation object rather than `volatile`, per esp32 guide §1/§9.

---

## 3. The command channel

### Replace the per-token matchers with one line reader

Both boards currently carry a separate static match index per token, hand-rolled
per call site (`main.cpp:90-127`, `ui_screenshot.cpp:213-244`). That pattern does
not survive a fourth and fifth command: the state multiplies, and the two
implementations have already diverged in whether a mid-token mismatch can start
a fresh match.

Replace it with one line-buffered reader per board that accumulates to `\n` and
dispatches on prefix, and put the dispatch table in
`firmware/shared/debug/console_command.h` next to `logging_config.h`, host-tested
in the same tree. `WAVEX-LOG` becomes a row in that table rather than a special
case.

**`WAVEX-ENTER-DFU` keeps its existing substring matcher and stays outside the
harness.** Two reasons: `scripts/daisy_dfu_trigger.py:45` writes the token with
**no trailing newline**, so a line reader would never fire it; and it must
survive into release builds because it is the only reflash path that does not
require someone physically holding BOOT+RESET. Gating the no-touch recovery
mechanism behind the debug profile is exactly backwards. (The script should gain
a newline anyway, for robustness — but the firmware should not depend on it.)

### Grammar

```
WAVEX-DBG <seq> <verb> [args...]        # host -> board
WAVEX-DBG: <seq> OK [key=value ...]     # board -> host
WAVEX-DBG: <seq> ERR <reason>           # board -> host
```

`<seq>` is a host-chosen integer echoed verbatim. **This is the single most
important difference between a debug console a human types at and a harness a
test suite drives**: the host never sleeps-and-hopes, it waits for the ack
carrying its own sequence number. It makes the existing silent-drop behaviour
(§1 item 3) reportable instead of invisible — a dropped command returns
`ERR busy` and the test fails honestly rather than flaking somewhere later.

**ESP32 verbs**

| Verb | Effect |
|---|---|
| `KEY <name> <PRESS\|RELEASE\|TAP>` | `KeyPress`/`KeyRelease` for any `PanelKey` by name into `InputDispatcher` |
| `ENC <delta>` | `EncoderLeft`/`EncoderRight`, magnitude in `delta` |
| `POT <delta>` | `EncoderUp`/`EncoderDown` |
| `TAP <x> <y>` | Press-hold-release through the synthetic indev |
| `TOUCH <DOWN\|MOVE\|UP> <x> <y>` | Individual pointer transitions, for drags |
| `STATE` | Machine-readable UI snapshot (§4) |
| `SCREENSHOT` | Existing capture, moved under this grammar |
| `LOG <...>` | Existing level control, moved under this grammar |

**Daisy verbs**

| Verb | Effect |
|---|---|
| `MSG <type_hex> <payload_hex>` | Straight into `ProcessInterMcuMessage` |
| `NOTE <note> <vel>` | Convenience wrapper over `MSG` for the common case |
| `STATE` | Voice/underrun/allocator snapshot (§4) |
| `LOG <...>` | Existing level control |

`MSG` accepting arbitrary bytes is deliberate: malformed-payload handling is
exactly the class of defect `make test-asan` was added for, and on-target fault
injection reaches the paths the host tests cannot construct.

### Real-time discipline

Unchanged from the existing tokens, and non-negotiable per daisy guide §1/§6 and
esp32 guide §3:

- The Daisy USB ISR copies bytes into a fixed buffer and publishes one
  release-store. No parsing, no dispatch, no logging in ISR context.
- Parsing and `ProcessInterMcuMessage` run on the **main loop**, never the audio
  callback.
- The line buffer grows from 48 to ~192 bytes for `MSG` payloads. Over-length
  input returns `ERR toolong` — never truncates into a valid-looking command.
- If the harness injects faster than the main loop drains, commands are dropped
  and counted, never blocked on (daisy guide §11).

---

## 4. Observability: `STATE`, not log scraping

Log output is prose, interleaved from several tasks, and its ordering under load
is not a contract. Asserting on it produces exactly the flaky suite that gets
disabled six months later. The harness therefore exposes one query returning a
flat `key=value` line — cheap to format on-target, trivial to parse, and stable
under log-level changes:

```
WAVEX-DBG: 7 OK page=SampleEdit depth=2 shift=0 sk0=Audition sk1=<Param \
           dropped=0 envelope_chunks=142 fps=27
```

Everything needed is already reachable and needs no new bookkeeping:
`UINavigator::active()->name()`, `depth()`, `isShifted()`
(`ui_navigator.h:27,31,44`), `UIPage::getSoftkeys()` (`ui_page.h:26`),
`InputDispatcher::droppedEvents()` (`input_dispatcher.h:27`), and the counters
the diagnostics page already renders.

**Threading**: reading navigator and page state touches LVGL-owned objects, so
`STATE` follows the screenshot state machine exactly — the console task raises a
request, the **UI task** fills the buffer under the port lock, the console task
prints it (`ui_screenshot.cpp:46,282-319`). The rule that nothing outside the UI
task touches LVGL has been broken twice and froze the display both times
(roadmap § Cross-Cutting Rules); the harness does not get an exception.

On the Daisy, `STATE` is plain main-loop context and replies through the log
ring like every other line.

Screenshots remain available and complement this: `STATE` is for assertions,
screenshots are for diagnosing a failure after the fact and for deliberate
visual-regression checks.

---

## 5. Compile-out

The [build profiles](../../README.md#build-profiles) already provide the
release-hygiene boundary this harness needs. What matters here is only the
contract between the two:

- **`WAVEX_DEBUG_HARNESS_ENABLED`** gates everything in §2–§4 on both boards:
  the line reader, the injection entry points, the synthetic indev, and the
  `STATE` formatter. It defaults from the `WAVEX_BUILD_DEBUG` master, and stays
  individually overridable so a release image can carry the harness for a
  bring-up session without turning logging back on.
- **`WAVEX-ENTER-DFU` is outside the guard**, per §3 and the build-profile
  guide.
- **The CI proof is a `strings` check for `WAVEX-DBG` in the release image**,
  not a successful compile — the command token is a string literal that cannot
  survive without the code referencing it, so its absence is real evidence that
  the harness is gone. A build that merely succeeds proves nothing about a call
  site someone added outside the guard.

One consequence worth stating here rather than in the build doc: the harness is
a genuine removal (a task, its buffers, an indev), but **the runtime log-level
table is not** — 12 bytes and a byte-load per call site survive into release,
because no compiler can prove the command channel that writes them is gone. The
logging saving comes from lowering the compile-time ceilings, not from this
flag. The release-ceiling decision is tracked in [`backlog.md`](../backlog.md).

## 6. The HIL suite

### Shape

```
tests/hil/
├── conftest.py          # port discovery, session-scoped target fixtures
├── wavex_target.py      # the driver: send/ack, state(), expect_log(), screenshot()
├── test_ui_nav.py       # page routing, softkeys, shift
├── test_sample_edit.py  # the envelope-cache and waveform rows
├── test_link.py         # inter-MCU round trips, reboot recovery
└── test_soak.py         # long-running; not in the default run
```

Host-side Python, matching `scripts/` (stdlib + pytest; port discovery reuses
`scripts/serial_ports.py`, which already resolves both boards by VID:PID).

Markers gate what each test needs: `@pytest.mark.daisy`, `.esp32`, `.both`,
`.sdcard`, `.slow`.

### Running it

**`make test-hil`, from a `./devcontainer.sh` shell with both boards
attached** — the same rule as every other build and test in this repo, and for
the same reason: the container is where the toolchain and the Python
environment actually are.

Three consequences worth stating, because each is a way to waste an afternoon:

- **The bare `docker run` one-liner in `AGENTS.md` will not work.** It mounts
  the repo and nothing else. HIL needs the USB access and serial group/device
  forwarding that `./devcontainer.sh` sets up, so that script is the supported
  entry point here, not an optional convenience.
- **It does not run in CI.** No CI runner has a Daisy and an ESP32 hanging off
  it. `make test` stays board-free and CI-safe; `make test-hil` is a bench
  command, run deliberately. That makes it the gate for a *phase*, not for a
  commit — which is the right altitude for it anyway, since the roadmap's phase
  gates are exactly the bench procedures §7 replaces.
- **It skips, silently and successfully, when no board is enumerated.** Running
  `make test-hil` on a laptop must be a no-op rather than a failure, or nobody
  will run it at all. `scripts/serial_ports.py --present` already answers this
  question and is what the fixture should call.

### The driver

```python
esp = target("esp32")
esp.key("MENU", "TAP")                       # returns only when the board acks
esp.enc(+3)
assert esp.state()["page"] == "SampleEdit"
esp.wait_state(envelope_chunks=lambda n: n > 0, timeout=2.0)
```

Every call is ack-synchronous, which removes the `time.sleep()` guesswork that
makes serial test suites flaky.

**Port ownership is the operational trap.** The existing scripts deliberately
write to the port *without* reading it, because `serial_log.py` holds the read
side and they tail `logs/<board>.log` instead (`wavex_log.py:72,79-93`). A HIL
runner must own both directions to read acks, so it takes the port exclusively,
fails fast with a clear message if `serial_log.py` already holds it, and writes
its own capture to `logs/hil-<run>.log` so a failure is diagnosable after the
fact.

### What it cannot test — read this before trusting a green run

- **The input drivers themselves.** Injection enters at
  `InputDispatcher::post()` and at a second indev, so TCA8418 decode, GT911
  coordinate handling and debounce are all *upstream* of every test here. The
  roadmap's "Keypad and encoder after the E-KEY/E-ENC fixes" row still needs a
  finger. What the harness does cover is everything downstream — which is where
  those fixes' *consequences* were.
- **Anything audible.** No test here can hear pitch, zipper noise, a click at a
  loop seam, or filter character. Those rows stay manual until there is a
  capture path.
- **Human-rate timing.** Injection runs at console-task cadence, not the 30 Hz
  UI poll, so tests drive the UI faster than a person can. Good for finding
  races; it will also miss ones that only appear at human speed, and can invent
  ones that cannot happen in the field.

---

## 7. Roadmap rows this automates

From § Outstanding hardware verification, the rows that become tests rather than
bench procedures:

| Row | Test |
|---|---|
| Diagnostics telemetry round trip | Navigate to Diagnostics, assert `DIAG_PUSH` row non-zero |
| Sample Edit waveform after the envelope-cache abort fix | Open Sample Edit on a loaded sample, assert `envelope_chunks > 0` within timeout — and, because the harness can repeat it 200×, actually identify the intermittent first-event cause the row says is still unknown |
| The August 2026 UI fix batch | Tab switching *by tap* (the exact path the row calls out), WAV duration past 97 s, encoder direction, audition/zoom without freeze |
| UART full-duplex DMA + IRQ priorities | Sustained `MSG` injection during SD streaming; assert counters and no drops |
| Both-MCU reboot recovery (cross-cutting gate) | Reset one board, assert the link re-establishes and `STATE` recovers |
| SD soak on libDaisy v8.1.0 | `@pytest.mark.slow` loop of load requests via `MSG` |
| LVGL port-lock hold time | Harness supplies the fast-encoder burst; `scripts/sysmon_stats.py` supplies the measurement |

Partially: Settings ▸ Display brightness (can drive the sweep and assert no I2C
error and a responsive UI; cannot see the panel dim). Not covered: the analog
path, MIDI latency, SVF cost and sound, DTCM placements — all need a scope, an
ear or a capture path.

---

## 8. Implementation order

Each stage is independently useful and independently committable.

1. **Shared line reader + `WAVEX-DBG` grammar with acks**, host-tested, both
   boards, existing commands moved onto it.
2. **ESP32 key/encoder injection** — smallest real capability, needs only
   `InputDispatcher::post()`.
3. **`STATE` on both boards.** With 2 and 3 the first meaningful HIL tests exist.
4. **Synthetic indev**: tap, then drag.
5. **Daisy `MSG` injection.**
6. **`tests/hil/` and `make test-hil`**, starting with the §7 rows.

## 9. Decisions

1. **`STATE` is extensible per page** — taken 2026-09-04, on the first real
   test: the Load-to-Track workflow's evidence (the picker prompt, the selected
   file, the last load's id) lives in the browser, and the Sample Manager's in
   its status line. `UIPage::consoleState()` appends a page's own `key=value`
   pairs; `UIPage::consoleCommand()` gives it a `PAGE <args>` verb. Both
   default to nothing. The tab host forwards to its live child and adds the
   tab titles and their button centres.

Settled while writing this:

- **Where HIL runs**: from a `./devcontainer.sh` shell with both boards
  attached, not CI. See § 6 *Running it*.
- **What C++ standard the shared parser may use**: C++17. `AGENTS.md` claimed a
  C++14 floor for `firmware/shared/`; that was stale — the Daisy device build
  and all three test trees are C++17, and `logging_config.h:36` `static_assert`s
  on it. AGENTS.md has been corrected.

## 10. As built

### The grammar and both boards

`firmware/shared/debug/console_command.h` — the `LineReader`, `ParseCommand`,
the reply builders and the hex/token helpers — is the one parser both boards
use, pinned by `firmware/shared/tests/debug/console_command_test.cpp`. The
marker is found anywhere in the line, not only at its start: the first line
after a port (re)opens can carry bytes already in flight, and the very first
Daisy PING of the bench run was lost to exactly that before the change.

The legacy seq-less lines (`WAVEX-LOG`, `WAVEX-FILTER`, `WAVEX-SCREENSHOT`)
still work and still answer in their legacy form, so `scripts/wavex_log.py`,
`wavex_filter.py` and `esp32_screenshot.py` are unchanged. `WAVEX-ENTER-DFU`
keeps its own substring matcher, as §3 required.

**ESP32** (`components/ui/src/ui_console.cpp`, `ui_console.h`; the console
task replaced the listener that lived in `ui_screenshot.cpp`):

| Verb | Effect | Answered from |
|---|---|---|
| `PING` | ack | console task |
| `LOG <...>` / `SCREENSHOT` | as before | console task |
| `KEY <PanelKey name> [PRESS\|RELEASE\|TAP]` | `InputDispatcher::post()`, posted as the keypad task posts a matrix key; `SOFT1`..`SOFT6`, `SAMPLE`/`PLAY`/`INSTRUMENT`/`TRACK`/`MIXER`/`SETTINGS`, `TRACK_PREV`/`TRACK_NEXT`, `PLAY_STOP`, `REC`, `PAD1`..`PAD16`, plus `SELECT`, `BACK`, `ENC`, `SHIFT` | console task |
| `ENC <±n>` / `POT <±n>` | one event carrying the magnitude, as the UI task's poll does | console task |
| `TAP <x> <y>` / `TOUCH <DOWN\|MOVE\|UP> <x> <y>` | the synthetic pointer indev; `TAP` holds PRESSED for three read cycles | console task |
| `STATE` | `page depth shift root lastkey track tstate tid tname sk0..5 sk<i>en sk<i>xy dropped` + the page's own pairs (`tab`, `tab<i>xy`, `status`, `sel`, `dir`, `entries`, `picker`, `target`, `lastid`, `rows`, `focusid`, `selidx`, …) | UI task, under the LVGL lock |
| `TRACK <n>` | selects a Track and asks the Daisy for its binding | UI task |
| `HOME` | unwinds to the main menu in one step (`UINavigator::popToRoot`) | UI task |
| `PAGE <args>` | the live page's `consoleCommand`: `TAB <title>` on a tab host; `DIR <path>` and `SEL <name>` on the Sample Browser | UI task |

Coordinates in `TAP`/`TOUCH` and in `sk<i>xy`/`tab<i>xy` are screen
(rotated) coordinates; the indev's `read_cb` inverts
`lv_display_rotate_point()` so LVGL lands the tap where `STATE` said the
button is. `TRACK`, `HOME`, `DIR` and `SEL` are set-up verbs, one layer above
the injection point on purpose: a test of Load should not also be a test of
scrolling an unknown card by pot detent.

**Daisy** (`daisy/src/main.cpp`; snapshots in `audio_engine.h` under
`WAVEX_DEBUG_HARNESS_ENABLED`):

| Verb | Effect |
|---|---|
| `PING`, `LOG`, `FILTER` | ack / as before |
| `STATE` | `voices underruns samples streaming blocks dropped` |
| `TRACKS` | `t0..t15` = `empty` / `sample:<id>` / `instrument:<name>` / `loading:<name>` — what `MSG_TRACK_BINDING` would say |
| `SAMPLES` | `n ids` — the WAV registry |
| `MSG <type_hex> <payload_hex>` | straight into `ProcessInterMcuMessage` |
| `NOTE <track> <note> <vel> [ON\|OFF]` | the wire's own `NoteMessage` |

### The suite

`tests/hil/`: `wavex_target.py` (driver), `conftest.py`, `test_console.py`,
`test_ui_nav.py`, `test_load_to_track.py`. `make test-hil` checks a board is
enumerated (else exits 0), starts the loggers if they are not running, and
runs pytest with the system interpreter (the IDF venv has no pytest).

**Transport deviates from §6.** The driver does not take the port exclusively:
it writes the command without claiming the port and reads the reply from the
serial logger's file, exactly as `wavex_log.py` does. That keeps the loggers
running through a HIL run — every board line, harness traffic included, lands
in `logs/<board>.log` — and the runner writes its own command/reply transcript
to `logs/hil-<run>.log`. The cost §6 feared is handled by `Target.probe()`:
a target is only used if a PING is answered *and* the log file moved, which is
the stale-logger symptom the bench notes describe. `LogTail` follows the file
across the loggers' rotation.

Encoder injection sends one event per detent by default (`enc(+3)` is three
`ENC 1`s): the main menu steps once per event whatever magnitude it carries,
and a slow turn is what a test means. `enc(n, steps=False)` posts one event
with the whole magnitude, as a fast spin would.

### What the first run found

Three defects, all fixed in the same change, none of which the host suites
could have seen:

- **The physical Back key acted as Select.** No page distinguishes button ids,
  so `ButtonPress` from `BUTTON_BACK` was "activate" on every page that handles
  presses. `InputDispatcher` now consumes Back globally and pops, the way it
  already handled Shift.
- **A rebooted peer wedged the link early in a session.** `SequenceTracker`
  only classified a fresh, out-of-tolerance sequence number as a reboot after
  100 frames of prior progress; an ESP32 reflashed after ~20 frames had every
  request dropped (`seqdrop` climbing on the Daisy) until its counter climbed
  back into tolerance. The prior-progress requirement is gone: the links are
  CRC'd point-to-point serial, where a seq 3 arriving against an expected 16
  is a reboot, not reordering.
- **The frontend's Track-binding cache went stale.** The Daisy only reported a
  binding when asked, so a Track that changed behind the UI's back (an
  unload, an eviction, an import completing) was still shown as it had been.
  The Daisy now pushes `MSG_TRACK_BINDING` on every change, the Browse tab
  warms the whole cache on entry, and `evict_oldest_loaded_sample()` drops the
  evicted id's bindings the way `UnloadSample()` already did — it had been
  leaving a Track's zone pointing at freed memory.

Not built: nothing from §2–§6 is missing. `test_soak.py` and the §7 rows
beyond the reboot-recovery and link ones are the open work.
