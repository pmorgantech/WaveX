# Debug Harness & Hardware-in-the-Loop Testing

**Status**: Proposed. Nothing in this document is built. The console channel it
extends *is* built and is the model for everything here — see
[`logging.md`](../logging.md).

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
| `WAVEX-ENTER-DFU` | Daisy | `daisy/src/main.cpp:64,96-104` | No, and deliberately so ([`build-profiles.md`](build-profiles.md) §2) |
| `WAVEX-LOG <...>` | Daisy | `daisy/src/main.cpp:77-128, 512-526` | **No — an unintended gap; closed by [`build-profiles.md`](build-profiles.md) §2** |
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
| `KEY <name> <PRESS\|RELEASE\|TAP>` | `ButtonPress`/`ButtonRelease` into `InputDispatcher` |
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

The build profiles this needs are specified separately, in
[`build-profiles.md`](build-profiles.md) — they close a release-hygiene gap that
exists today and are worth doing whether or not this harness is ever built. What
matters here is only the contract between the two:

- **`WAVEX_DEBUG_HARNESS_ENABLED`** gates everything in §2–§4 on both boards:
  the line reader, the injection entry points, the synthetic indev, and the
  `STATE` formatter. It defaults from the `WAVEX_BUILD_DEBUG` master, and stays
  individually overridable so a release image can carry the harness for a
  bring-up session without turning logging back on.
- **`WAVEX-ENTER-DFU` is outside the guard**, per §3 and `build-profiles.md` §2.
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
flag. `build-profiles.md` §3 has the argument and the measurement it still owes.

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

0. **[`build-profiles.md`](build-profiles.md) in full** — the flag hierarchy,
   the Daisy `WAVEX-LOG` guard, both profiles, the CI `strings` gate. It is
   stage zero because it is a prerequisite for everything below *and* because it
   is worth doing even if nothing below it is ever built. It contains no part of
   this design.
2. **Shared line reader + `WAVEX-DBG` grammar with acks**, host-tested, both
   boards, existing commands moved onto it.
3. **ESP32 key/encoder injection** — smallest real capability, needs only
   `InputDispatcher::post()`.
4. **`STATE` on both boards.** With 3 and 4 the first meaningful HIL tests exist.
5. **Synthetic indev**: tap, then drag.
6. **Daisy `MSG` injection.**
7. **`tests/hil/` and `make test-hil`**, starting with the §7 rows.

## 9. Decisions still open

1. **Should `STATE` be extensible per page?** A virtual `UIPage::debugState()`
   would let the Sample Edit page report marker positions. Powerful, but it puts
   a debug method on every page's interface — and it is only worth it if tests
   actually need per-page internals that softkeys and page name do not give.

Settled while writing this:

- **Where HIL runs**: from a `./devcontainer.sh` shell with both boards
  attached, not CI. See § 6 *Running it*.
- **What C++ standard the shared parser may use**: C++17. `AGENTS.md` claimed a
  C++14 floor for `firmware/shared/`; that was stale — the Daisy device build
  and all three test trees are C++17, and `logging_config.h:36` `static_assert`s
  on it. AGENTS.md has been corrected.
