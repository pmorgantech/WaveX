# Panel Controls — Buttons, LEDs, Endless Pots, and MIDI I/O

**Status**: Design with stages 0–4 and MIDI port output implemented; panel firmware added 2026-09-17.
Physical panel validation remains open. Pin numbers live only in `firmware/shared/config/pin_config.h`; feature
flags and table sizes only in `hardware_config.h`. This document names the
functions those pins carry and the rules that produced the allocation — never
the numbers.

**Where it fits**: Phase 2 of `roadmap.md`. The Phase 2 gate is "program and
perform a four-track pattern with swing *from the panel*", item 2 needs DIN
MIDI pins that do not collide with the flash port, and item 3 (pad grid, LED
feedback) needs the drivers and the input model designed here. It is therefore
a Phase 2 prerequisite, sequenced before items 2 and 3 as "2.P".

---

## 1. Current implementation (as-built, 2026-09-17)

What the ESP32 image actually does with the panel today, from code (file
references are the audit trail; re-verify before trusting):

| Piece | State |
|---|---|
| Touch (GT9271 via GT911 driver) | The only `lv_indev` fed by hardware. Bus is the BSP's I2C (`bsp_i2c_get_handle()`), shared with the keypad. |
| Encoders | Two PCNT units, 4x quadrature decode, glitch filter, polled by `panel_task` with a 2 ms delay plus I/O (`main/pcnt_task.cpp` supplies the service routine). Unit 0 posts raw counts; unit 1 divides by the detent constant. Both reach pages as `InputEvent`s. The bench encoder is unit 1 (confirmed 2026-09-05); unit 0 has nothing wired, and its channel B had pointed at a GPIO that is not on the board's header. Unit 1 counts negative on clockwise as wired; direction is now one per-encoder setting in `hardware_config.h` (`WAVEX_*_DIRECTION`) applied in the PCNT task, and the three pages that had compensated were reverted to the shared `steps()` contract (2026-09-05). No encoder push handler exists; "encoder click" is TCA8418 keycode 3. |
| Keypad (TCA8418) | INT wakes a bounded FIFO task; 100 ms safety poll, 10 ms fallback without INT. Logical key map and diagnostics exist. Geometry and wiring remain unverified; see HV-011. |
| Softkeys | Six on-screen buttons, touch only. `SoftkeyBar::focusNext()` / `pressFocused()` exist with no callers — the documented "encoder scrolls softkeys" interaction is not in the binary. |
| Shift | Latched-and-sticky global modifier in `InputDispatcher::processAll()` with a header chip. Works, driven by keycode 4 today. |
| LEDs (temporary TLC5947) | `panel_task` owns SPI2 DMA and complete-frame latching; logical policy and diagnostics implemented. HV-012 open. |
| Pot ADC (MCP3208) | Eight-channel DMA scans, RV112FF 20 kΩ decoder, calibration and page bindings implemented; HV-013 open. |
| DIN MIDI | Compiled out since 2026-09-04: RX sat on the USB-Serial/JTAG D- pin. Pins moved 2026-09-05; still off until rewired (§5). UART2 RX/TX rings and clock/transport serializer implemented; enable only after wiring confirmation (HV-014). |
| USB MIDI | Saved Device/Host selection on the **USB 2.0 High-Speed OTG** controller (`TINYUSB_DEFAULT_CONFIG()` selects the HS port on the P4), i.e. the board's 4-pin USB connector, independent of the flash port. Note input plus queued clock/transport output; input/output flags work independently. Enumeration and timing remain unverified (HV-014, HV-035). See [port roles](#usb-midi-port-roles-2026-09-23). |
| Input plumbing | `InputEvent` → `InputDispatcher` queue (64 deep) → drained on the UI task under the LVGL lock → global Shift/Back → `UIPage::onInput()`. The debug console injects the same events (`KEY`, `ENC`, `POT`). |

The good news is that everything downstream of `InputDispatcher::post()` is
already generic. The panel work is (a) drivers, (b) a real logical-key and
LED namespace, and (c) the page contract for pots — not a UI rewrite.

## 2. Parts: reassessment

| Part | Verdict | Why |
|---|---|---|
| **TCA8418** keypad controller (I2C) | **Keep.** | Up to 80 keys on two wires plus INT, hardware debounce, 10-event FIFO; already on the touch I2C bus and already driven. Every panel key, both encoder push switches and the 16 Phase-2 pads fit in one part with rows to spare. Two caveats: the vendored driver hard-codes 100 kHz for its device (fine — events are tiny), and it cannot do velocity. Pads are on/off switches in this design; velocity comes from touch position or a fixed level, as required by the panel's switch-only hardware. |
| **TLC5947** 24-ch 12-bit constant-current LED driver | **Temporary backend, two chained (48 ch).** | User decision 2026-09-17: use TLC5947 for bring-up, then replace it with PCA9956B. Keep policy and logical brightness independent of chip registers and bus. It has no chip select — it is a shift register — which is the one rule the SPI2 driver has to respect (§3.3). If per-pad **RGB** is ever wanted, 16 pads alone need 48 channels; switch to an I2C matrix driver (IS31FL37xx class) then rather than chaining four TLC5947s. Not a v1 concern. |
| **MCP3208** 8-ch 12-bit SPI ADC | **Adopt**, user confirmed 2026-09-17. | Eight channels serve four dual-wiper pots. Commands and result extraction explicitly implement the MCP3208 wire format. Use a conservative acquisition clock for the unbuffered 20 kΩ pots; settling remains a bench gate. |
| **PEC11R** detented quadrature encoders (x2, PCNT) | **Keep both** as navigation encoders. | Already working through the hardware pulse counter, glitch-filtered, no CPU cost. Detents suit list navigation and value stepping; the push switch gives Select. The endless pots are a different tool (§2.1). |
| **CD74HC4067** analog mux | **Dropped** (removed from `hardware_config.h` 2026-09-05). | Predated the MCP3208; its plan used the chip's single ADC through a mux on address pins that are not on this board's header. |
| Alpha **RV112FF, 20 kΩ**, four | User confirmed 2026-09-17. | Endless dual-unit potentiometers. The decoder assumes quarter-turn-offset triangular wipers; measure the actual waveform before enabling controls (HV-013). |

### 2.1 Why both encoder types

An endless pot is *not* an encoder with more resolution. It is two analog
wipers whose values give the knob's absolute angle within a turn; the
firmware differentiates that into a delta. It has no detents, turns freely,
and reads smoothly at any speed — the feel of a parameter knob. A detented
quadrature encoder is the feel of a *selector*: one click, one item. WaveX
wants both, in the places each is right:

- **Nav encoders (2, PCNT)** — `NAV_A` beside the screen: list/focus
  movement, value stepping, push = Select. `NAV_B`: secondary axis (zoom in
  Sample Edit, Track select under Shift per `track-and-patch-model.md` §6).
- **Parameter pots (4, endless)** — under the screen, one per column of a
  4-wide parameter strip the active page defines (§4.3). Turn to change the
  value shown above the knob. Shift + turn = fine.

If the bench proves only one kind is wanted, the other's stage simply never
lands; the input model does not care.

## 3. Hardware plan

### 3.1 The port budget

The Waveshare ESP32-P4-WIFI6 exposes 27 GPIO on its headers. This is the
whole budget; everything else (MIPI, USB HS pair, TF-card SDMMC, C6 SDIO) is
on-board. The allocation in `pin_config.h` follows these rules, in priority
order:

1. **Never claim a USB pin as GPIO.** One full-speed pair is the
   USB-Serial/JTAG flash and debug port (the cause of the 2026-09-04 MIDI
   storm). The *other* full-speed pair is left free, reserved for a future
   USB **host** port — USB-stick sample import (Phase 5) or a USB MIDI
   controller. Two GPIO spent on optionality, deliberately.
2. **Keep what is wired.** Inter-MCU UART, the BSP I2C bus, PCNT unit 1, the
   keypad INT stay where they are.
3. **Keep the dormant SPI-slave link's five pins reserved** until the SPI
   revival decision (`roadmap.md`) is made. They cost the panel nothing it
   needs today; releasing them is a one-line decision later.
4. **Adjacent header pins for each pair** (encoder A/B, MIDI RX/TX, the SPI2
   run) so each control is one small connector.

Under those rules the panel needs nine pins and gets them, with **one spare**
(earmarked for a second MCP3208 chip select if more than four pots or any
plain pots are ever added). The result is in `pin_config.h`, with the
reasoning for each move recorded there.

### 3.2 Bus topology and ownership

```
ESP32-P4
├─ I2C (BSP bus, 400 kHz)      GT9271 touch + backlight ── TCA8418 keypad (100 kHz device clock) + INT
├─ SPI2 master                 TLC5947 #0 ─ TLC5947 #1 (chain; XLAT, BLANK)   MCP3208 (CS)
├─ PCNT unit 0, unit 1         NAV_A, NAV_B quadrature (push switches → TCA8418 matrix)
├─ UART1                       inter-MCU link (as-built)
├─ UART2                       DIN MIDI in/out
├─ USB 2.0 HS OTG (4-pin conn) USB MIDI device (as-built) — to the DAW
├─ USB-Serial/JTAG (Type-C)    flash / debug — never touched by WaveX code
└─ USB 1.1 FS pair             reserved, unpopulated: future USB host
```

**One task owns SPI2.** The MCP3208 reads and the TLC5947 frames go through
the same bus from the same task (`panel_task`, §4.1), so there is no bus
mutex and no way for a pot read to interleave with a half-shifted LED frame.

### 3.3 The TLC5947 rule

The TLC5947 has no chip select. Every clock edge on SPI2 — including the
MCP3208's — shifts data through its 288-bit register. That is harmless only
because outputs change on **XLAT**, not on shift. Therefore:

- The driver always sends the **complete** 72-byte chain frame immediately
  before pulsing XLAT, never a partial update.
- BLANK is held high (all outputs off) from power-on until the first frame is
  latched — via a pull-up so the LEDs are dark before the firmware runs, then
  driven by the GPIO. BLANK also gives free global dimming/screen-blanker
  integration.
- SPI2 carries two `spi_device` handles with separately configured clocks;
  see `hardware_config.h`. ADC reads never pulse XLAT. The panel task owns bus
  lifetime separately from both devices, so LED retries do not remove the ADC bus.

### 3.4 Control inventory (v1)

Keys (all TCA8418 matrix; the count below fits comfortably inside the part's
maximum — `WAVEX_TCA8418_ROWS/COLUMNS` follow the panel PCB, not this table):

| Group | Keys | LED |
|---|---|---|
| Softkeys | `SOFT1`…`SOFT6`, directly under the screen's six softkey buttons | one each (dim = defined, bright = latched/active state) |
| Modifier | `SHIFT` | one (mirrors the header chip) |
| Menu jumps | `SAMPLE`, `PLAY`, `INSTRUMENT`, `TRACK`, `MIXER`, `SETTINGS` — the root groups of `ui-architecture.md` "Navigation structure" | one each (lit = the active root group) |
| Navigation | `BACK`, `NAV_A_PUSH` (Select), `NAV_B_PUSH` | — |
| Track | `TRACK_PREV`, `TRACK_NEXT` | — |
| Transport | `PLAY_STOP`, `REC` | one each (Phase 2 semantics) |
| Pads | `PAD1`…`PAD16` (Phase 2 pad grid; wire now, semantics later) | one each |

That is 37 keys and 31 LEDs, inside `WAVEX_LED_CHANNELS` with room spare.
`hardware_config.h` sizes (`WAVEX_LED_CHANNELS`, rows/columns) follow the
final panel PCB, not this table.

Analog: four RV112FF 20 kΩ endless pots, each with two wiper outputs → eight
MCP3208 channels. Connect the resistive elements across the same supply/reference
as the ADC and measure both wipers. Confirm terminal identification from the
manufacturer drawing and the actual parts before powering the assembly.

### 3.5 MIDI hardware

- **DIN in**: standard 5 mA current-loop receiver (6N138 or H11L1 class
  optocoupler, 220 Ω series) into UART2 RX at 3.3 V. This is what the storm
  detector exists to protect; with the receiver present the line idles high.
- **DIN out**: 3.3 V MIDI out per the MIDI CA-033 spec (10 Ω to 3.3 V on
  pin 4, 33 Ω from TX on pin 5), buffered.
- **USB MIDI**: the 4-pin "V D- D+ G" connector is the HS OTG port and is
  already what the firmware enumerates on. Nothing to wire beyond a lead to
  a panel USB-B/C socket. **Do not** put a USB device on the reserved
  full-speed pair without a design change here.

## 4. Software stack

### 4.1 Ownership and tasks

```
panel_task (prio 5, 2 ms delay + I/O)            keypad_task (existing → INT-driven)
├─ PCNT unit 0/1 deltas               └─ TCA8418 FIFO on INT, 100 ms fallback poll
├─ MCP3208 8-ch DMA scan                 │
├─ endless-pot decoder → bounded delta mailbox            │  physical keycode → PanelKey (table)
├─ LED frame flush if dirty                │
│      │  InputEvent (Pot/Encoder)         │  InputEvent (Key press/release)
│      └──────────────► InputDispatcher::post() ◄──────────┘
│                               │
│              UI task: InputDispatcher::processAll()
│              ├─ global keys: SHIFT, BACK, SOFTn, menu jumps, TRACK±, transport
│              └─ page: UIPage::onInput() / pot bindings
│
└─ ◄── LED frame (locked value mailbox) ◄── PanelLeds (UI-task-owned model)
```

- `panel_task` services PCNT, ADC and LEDs, then delays 2 ms. Its actual
  period includes transactions and occasional calibration persistence; measure
  latency and PCNT overflow headroom in HV-013. It is the **sole SPI2 user**
  and never touches LVGL. The UI drains at most four accumulated pot deltas
  per pass into the existing input queue; queue overflow uses its existing drop counter.
- The keypad task switches to the INT line (the TCA8418 `CFG` register's
  `KE_IEN` bit is written, INT falling edge → task notification), keeping a
  slow poll as a safety net. Physical latency remains unmeasured in HV-011; no sub-millisecond claim is made.
- The UI task owns all *meaning*: which key does what, what the LEDs show.
  It publishes a fixed-size LED frame under a short SMP lock; `panel_task`
  copies it, releases the lock and flushes only changes.
  The UI task never blocks on SPI.

### 4.2 Data model

Entities and where their truth lives:

| Entity | Shape | Source of truth |
|---|---|---|
| `PanelKey` | `enum class : uint8_t` — the logical keys of §3.4, replacing the four `BUTTON_*` constants in `ui_softkey.h` (kept as aliases during the transition) | `components/ui/include/ui/panel_key.h` |
| Key map | TCA8418 keycode (row·10 + col + 1) → `PanelKey`, one table, `static_assert` no duplicate keycode | `hardware_config.h` (it is wiring truth, like a pin) |
| `PanelLed` | `enum class : uint8_t` — the LEDs of §3.4 | `components/ui/include/ui/panel_led.h` |
| LED map | `PanelLed` → physical output channel index, one table, `static_assert` no duplicate channel and all `< WAVEX_LED_CHANNELS` | `hardware_config.h` |
| `InputEvent` | Existing struct gains `InputType::KeyPress/KeyRelease` carrying a `PanelKey` in `source_id`, and `InputType::PotUp/PotDown` carrying the pot index in `source_id` and the magnitude in `delta` — same magnitude-plus-direction contract `steps()` already enforces. Existing `Encoder*` types stay for the nav encoders. | `input_event.h` |
| `EncoderBinding` | Four fixed records: label, owned value text, owner/callback, parameter index, coarse multiplier and enabled flag | `ui/encoder_binding.h` |
| LED frame/policy | Fixed array of chip-independent 8-bit brightness indexed by `PanelLed`, plus blank/test state; UI owns policy, output task owns applied status | `ui/panel/panel_led_frame.h`, `main/panel/` |
| Pot calibration | Per-wiper ranges, per-pot direction and enable; versioned explicit bytes in NVS namespace `wavex_panel`, key `pots_v1`; defaults disabled | `shared/panel/endless_pot.hpp`, `main/panel/pot_store.cpp` |

Invariants: every physical keycode maps to at most one `PanelKey`; every
`PanelLed` maps to exactly one channel; `SHIFT` and `BACK` never reach a
page (already true). Bindings are fetched from the active page at dispatch
time under the LVGL lock. The strip refreshes from the same binding values;
unchanged text does not invalidate widgets. No worker retains page pointers.

### 4.3 Key semantics (global, in `InputDispatcher::processAll()`)

| Key | Behaviour |
|---|---|
| `SHIFT` | Unchanged: latched, sticky, header chip; SHIFT LED mirrors `isShifted()`. |
| `BACK` | Unchanged: `UINavigator::pop()`, root stays put. |
| `SOFT1..6` | `SoftkeyBar::press(n)`: fires the active page's softkey *n*, honouring the shifted row exactly as a touch on that button does (calls `notifySoftkeyUsed()`). Reuses the deferred-callback path touch uses, so ordering relative to touch is identical. |
| Menu jumps | `UINavigator::jumpToRoot(group)`: pop to the main menu, push the group. No state is preserved across a jump — that is what makes it predictable. The lit jump LED is the group at the bottom of the stack. |
| `NAV_A_PUSH` | `BUTTON_SELECT` semantics (page-handled, as today). |
| `TRACK_PREV/NEXT` | Change the shared selected Track (`ui/current_track.h`); the header chip updates on every page. |
| `PLAY_STOP`, `REC`, `PAD1..16` | Forwarded to the page until Phase 2 gives them global sequencer semantics (`sequencer.md` §5). Until then a page that does not handle them drops them; the Diagnostics Panel tab shows them so wiring can be verified before they mean anything. |

Pot deltas invoke the active page's binding. The dispatcher applies its coarse
multiplier normally and one fine unit with Shift. Continuous Play and Instrument
controls use four fine units per coarse step; enum controls remain one choice
per step. Turning does not consume sticky Shift.

### 4.4 Endless-pot decoding (host-testable)

`firmware/shared/panel/endless_pot.hpp`, HAL-free, C++17, under
`firmware/shared/tests/`:

1. Normalize calibrated wipers to a 4096-unit turn. Reject invalid ranges and
   pairs inconsistent with the quarter-turn triangular model.
2. Use the wiper farther from its fold and the other wiper's half-range to
   reconstruct angle. Retain fractional movement for 64 logical steps/turn.
3. An eight-angle-unit dead band retains its anchor so slow turns accumulate.
   Implausible jumps over an eighth-turn or gaps over 100 ms rebase without an
   edit. Invalid reads clear accumulated input. There is no acceleration.
4. Defaults are disabled. Range capture, frozen-range clockwise verification
   and explicit Save enable each pot. Shape rejection is not a reliable probe
   for physical presence: disconnected analog inputs can float.

Host tests cover triangular sweeps, wrap, direction, slow movement, jitter,
stale/invalid readings, calibration acceptance, serialization, failed saves,
cancellation and reconnects. They do not establish the RV112FF waveform.

### 4.5 LED policy (v1)

Driven entirely from navigator/page state — no page sets an LED directly:

- `SHIFT` LED = `isShifted()`.
- Jump LEDs: the active root group bright, others off.
- Softkey LEDs: defined softkey dim, undefined off; a page may mark one
  softkey `active` → bright when enabled. The explicit `Softkey::active`
  flag currently covers Play Latch and Sequencer Play/Stop.
- Transport/pad LEDs: Phase 2 (step/playhead mirror; see `sequencer.md` §5).
- Screen blanker: `DisplayManager` blank → BLANK high (all LEDs off); any
  panel input wakes both.

### 4.6 Diagnostics, console, tests

- **Diagnostics → Panel** shows keypad mapping and LED tests/status.
- **Settings → Pots** shows raw wipers, decoded angle/validity, ADC errors and
  per-pot calibration. Select a pot, Start, turn twice to capture ranges, Verify,
  then turn clockwise through a full revolution and Save. Cancel leaves the
  previous calibration intact. Disable persists the disabled state.
- **Console**: `PANEL` reports ADC readiness/scans/errors, calibration status and
  `pot0`…`pot3` tuples (raw A, raw B, angle, valid, enabled). `POT <n> <delta>`
  injects through the same binding dispatcher; it does not require physical
  calibration. `KEY`, `STATE` and `LEDS` retain their existing panel diagnostics.
- **Host tests** cover the decoder/service, binding scaling and strip redraws;
  physical checks and HIL results remain open in HV-013.

### 4.7 MIDI software

- DIN: input/output share one UART owner with RX/TX rings. The receiver remains
  disabled by default until its wiring is confirmed. Existing note forwarding
  and the storm warning remain; no MIDI THRU is enabled.
- USB: one I/O task drains input and emits complete USB-MIDI event packets.
  Either input or output may be compiled independently. The RX callback signals
  a permanent semaphore, avoiding notifications to a deleted task during stop.
- `midi_out.h` accepts the existing clock-out message and a destination mask.
  Callers only enqueue values; each port task alone touches its output driver.
  The real packet router delivers `MSG_SEQ_CLOCK_OUT` to both ports.
- `MIDIOUT` console diagnostics and the bench procedure are in
  [HV-014](../hardware-validation.md#hv-014--midi-ports-and-clock-serialization).
  Daisy clock generation and ESP32 external-clock/SPP ingest are implemented;
  [MIDI sync](midi-sync-tempo-follower.md) defines transport/source behavior.
  Physical DAW synchronization remains unverified.

## 5. Stages (each one commit, each independently buildable)

| # | Stage | Host-verifiable | Bench |
|---|---|---|---|
| 0 | **Pin reconciliation** — `pin_config.h` rewritten against the WIFI6 header; CD74HC4067 removed; MIDI pins moved; per-encoder direction flags. *Done 2026-09-05.* | compiles | clockwise is forward on every page |
| 1 | **`PanelKey` / `PanelLed` model + key map** — enum, table in `hardware_config.h`, `InputEvent` extensions, `KEY <name>` console verb, dispatcher handling for `SOFTn`, jumps, `TRACK±`; `SoftkeyBar::press(n)`; `UINavigator::jumpToRoot()`. The dead `focusNext/pressFocused` deleted. *Done 2026-09-05.* | HIL: jumps, softkeys via key, Shift row (`test_panel_keys.py`) | keycode → key on the Diagnostics ▸ Panel tab |
| 2 | **Keypad INT** — implemented 2026-09-17: CFG, ISR notification, fallback, error recovery. | FIFO/configuration/race/backpressure tests | HV-011: latency, key rolls, shared-bus recovery |
| 3 | **LED output** — implemented 2026-09-17: temporary TLC5947 backend, PCNT service, replaceable output interface, blanking, policy and diagnostics. | Policy/packing tests and firmware compile; HIL still open | HV-012: mapping, startup, sleep, timing; shared-pot traffic covered by HV-013 |
| 4 | **MCP3208 + RV112FF 20 kΩ pots** — implemented 2026-09-17: decoder, NVS calibration, Settings → Pots, page bindings, strip, Shift fine mode. Play and Instrument Filter/Amp are first consumers. | Decoder/service/binding/widget tests and firmware compile | HV-013: waveform, acquisition settling, calibration, feel, latency and rendering |
| 5 | **MIDI ports** — output queues, UART TX ring, USB output and clock-out route implemented 2026-09-17. DIN enable awaits wiring. | Packet/queue/routing tests; enabled DIN and USB flag variants compile | HV-014: enumeration, I/O, lifecycle and latency; timing gate remains open |

**Gate**: from the panel alone (no touch), jump to Instrument, change the
filter cutoff on a pot and hear it, latch Shift and fire a shifted softkey,
return with BACK; all corresponding LEDs correct throughout; DIN and USB MIDI
notes sound; `make test` and `make test-hil` green.

## 6. Decisions taken here (revisit only with a reason)

1. Both encoder kinds: two detented nav encoders on PCNT, four endless pots
   on an MCP3208 (§2.1).
2. The second full-speed USB pair is reserved for a future host port; the
   dormant SPI-slave pins stay reserved (§3.1). Together that is seven GPIO
   held back, leaving one spare after the panel.
3. USB MIDI uses the HS OTG port in either Device or Host mode (2026-09-23 user-authorized Phase 2 addition) — the 4-pin connector is
   *the* USB MIDI port; the Type-C is only ever flash/debug.
4. Key and LED maps live in `hardware_config.h` alongside the other wiring
   truth rather than in a third config file.
5. SPI2 has one owner (`panel_task`); the UI task never performs bus I/O.
6. Pads are switches; velocity is not a panel-hardware feature in v1.

## 7. Out of scope / later

RGB pads; velocity-sensing pads; a second simultaneous USB host port (the
reserved FS pair, stack support and a power switch — Phase 5); MIDI THRU (hardware only, if
the panel PCB has room); moving MIDI DIN out to the Daisy if clock jitter over
the link proves too high (subject to the transport decision in `../architecture.md`).

## 8. Open hardware questions for the bench

Recorded in `roadmap.md` § Outstanding hardware verification:

- ~~Which encoder is physically wired, and to what?~~ Unit 1, confirmed 2026-09-05;
  direction fixed at the source; clockwise verified forward on every page
  the same day.
- TCA8418 matrix geometry (`WAVEX_TCA8418_ROWS/COLUMNS`, unconfirmed) and the
  `WAVEX_KEYCODE_*` map beyond the four bench keys — the Diagnostics ▸ Panel
  tab (stage 1, landed) shows each press's keycode, row/column and `PanelKey`.
- USB MIDI enumerates on the 4-pin HS connector — never confirmed on the
  bench (roadmap "MIDI latency" row).
- RV112FF 20 kΩ wiper waveform and relative phase — the decoder assumes
  quarter-turn triangular waves; verify on a scope before calibrating.


## Keypad INT implementation (stage 2, 2026-09-17)

`KeypadFifo` owns FIFO decoding and accepted-key state; the target adapter owns
one BSP I2C device and its task/interrupt lifetime. The old managed wrapper has
no non-destructive interrupt ACK or error-return API and aborts on bus errors;
the adapter therefore uses ESP-IDF's existing I2C driver directly. No new
production dependency is introduced.

The GPIO ISR only notifies the keypad task. Its endpoint is withdrawn under an
ISR-safe lock before task deletion; the task removes only its own handler,
never the BSP's shared ISR service. The handler and endpoint are in internal
IRAM/DRAM because the BSP may already have installed an IRAM interrupt service.
The task remains priority/stack-configured, pinned to core 1, and performs all
I2C with bounded transaction timeouts. It handles at most 16 FIFO events per
pass; busy/error paths yield. Missing hardware returns an initialization error
without aborting or removing the shared touch bus.

FIFO reads precede write-one-to-clear ACK, followed by a count recheck for an
event racing the ACK. A full input queue retains the undelivered event. Overflow
or bus failure releases accepted held keys and discards ambiguous FIFO history;
no fabricated new presses are emitted. Overflow mode and its interrupt enable
are both configured per the [TI erratum](https://www.ti.com/lit/ds/symlink/tca8418.pdf).
Shutdown retries outstanding releases before freeing the device, returning a
timeout if it cannot finish safely. Diagnostics → Panel shows INT/POLL, I2C
errors and overflow observations. [HV-011](../hardware-validation.md#hv-011--keypad-interrupt-and-recovery)
owns the unrun electrical, latency and shared-touch checks.


## LED output implementation (stage 3, 2026-09-17)

`PanelLedFrame` carries 8-bit logical brightness, blanking and an optional
physical-output diagnostic override. `BuildPanelLedFrame` derives values from
cached softkeys (dim when defined; bright when enabled and explicitly active),
Shift, root group and confirmed transport. Play reports held/latched pads;
Sequencer reports the selected Track's confirmed enabled steps dim and its
matching Pattern playhead bright. Recording mode lights Rec on Sequencer.
Off-context pads are dark. Tab hosts forward their active child's state.

`panel_task` absorbs PCNT polling and exclusively owns backend initialization,
writes and shutdown. It checks changed frames every 20 ms, separately from the
PCNT/ADC service loop (2 ms delay plus I/O). UI publication and status readback copy complete values under
a short SMP lock; no lock is held over I/O. A UI heartbeat older than one second
forces blanking. Screen sleep overrides all policy and diagnostic test output.

The temporary TLC5947 backend allocates one driver-aligned internal DMA buffer
at initialization. Every write packs the entire chain in descending channel
order, with 12-bit values MSB first, waits for ESP-IDF SPI DMA completion, then
pulses XLAT. Startup latches zeros before enabling outputs. Failure/shutdown
assert BLANK; failed initialization/writes retry at most once per second without
stopping encoder service. The external BLANK pull-up remains essential before
firmware starts. The [TI datasheet](https://www.ti.com/lit/ds/symlink/tlc5947.pdf)
and ESP-IDF SPI driver own timing and DMA requirements; pins/map/backend
selection remain solely in the canonical configuration headers.

The backend interface is `init/write/shutdown`; policy and pages never reference
TLC registers, SPI or PCA addresses. Selecting PCA9956B currently reports
`ESP_ERR_NOT_SUPPORTED` without claiming TLC control pins or transmitting LED
frames. The ADC may still own the shared SPI bus. Replacing it requires a
board-specific backend for address/current/output-enable setup and PWM writes,
plus the canonical channel map. The logical brightness/page contract is unchanged.

Diagnostics → Panel offers **LED walk** (next physical channel per press) and
**LED all/off** (toggle); tests expire after ten seconds or leaving that tab.
The card shows driver availability, blanking, test channel and error count.
Debug console `LEDS WALK`, `LEDS ALL`, `LEDS OFF` control the same test state;
`OFF` restores normal policy. Plain `LEDS` reports driver, ready/blank, test
(-1 normal, -2 all), errors, writes and `levels` (two hex digits per physical
channel in ascending order). This is the last successfully transported frame;
a just-requested test may not appear until the next UI/output service passes.
`STATE` adds `leddriver`, `ledready`, `ledblank`. Transport success cannot verify
the write-only chain or its wiring. [HV-012](../hardware-validation.md#hv-012--panel-led-output)
owns those checks, HIL and measured timing; no hardware result is claimed.


## Pot implementation (stage 4, 2026-09-17)

`panel_task` owns all ADC transactions, decoder state and NVS writes. Fixed-size
snapshots/commands cross a short SMP lock; no SPI/NVS operation runs under that
lock or the LVGL lock. Each channel uses a four-byte full-duplex DMA transaction
with preallocated aligned buffers; only a complete successful scan is published.
The SPI device and bus lifetimes are separate, including the PCA LED stub.

The acquisition clock is deliberately conservative for unbuffered 20 kΩ pots
(approximately 5 kΩ worst-case divider source resistance). Measure settling and
channel crosstalk before claiming twelve effective bits. ADC configuration and
channel assignments live only in the canonical config headers.

NVS stores explicit versioned bytes, validates every field and never erases the
partition automatically on errors. Runtime calibration changes only after a
successful commit. A failed Save leaves the verified candidate available for a
retry; Cancel leaves active settings intact. During calibration all pot edits
are suppressed. Missing or malformed stored data leaves the pots disabled.

Play binds Cutoff, Resonance, Attack and Decay. Instrument binds Cutoff,
Resonance, Type and Model on Filter, and Level/Pan on Amp. Other Instrument
sections leave their four slots empty. Each page reserves a 60-pixel strip inside
its existing content bounds. Hardware rendering and feel remain unverified.

## Related references

- [Hardware validation HV-013](../hardware-validation.md#hv-013--mcp3208-and-endless-pots)
- [Microchip MCP3208 data sheet](https://ww1.microchip.com/downloads/en/devicedoc/21298e.pdf)
- [Alpha RV112FF catalog](https://www.taiwanalpha.com/downloads?id=79&target=products)
  (mechanical/terminal drawing; no electrical phase waveform supplied)
- [ESP-IDF 5.5 SPI master](https://docs.espressif.com/projects/esp-idf/en/v5.5/esp32p4/api-reference/peripherals/spi_master.html)


## MIDI port implementation (stage 5, 2026-09-17)

`main/midi_out.cpp` owns two fixed 32-entry queues under a short SMP lock.
The shared HAL-free `midi/clock_output.hpp` validates/encodes the central
protocol message. Each queue carries complete bytes and an enqueue timestamp;
no output occurs under the lock. No new protocol type or production dependency
is introduced. The supported output messages are Clock, Start, Continue, Stop
and Song Position Pointer; note/CC output and MIDI THRU are not added here.

DIN's existing task owns both directions, priority/stack from `hardware_config.h`,
with at most 64 input bytes and one output message per pass. It waits at most one
RTOS tick for RX and checks previous TX completion before writing at most three
bytes into the TX ring. USB's existing task uses the same bounded pass, wakes on
RX or a one-tick timeout for output service, and uses TinyUSB's all-or-none packet
API. USB OUT is drained even when note input is disabled. Both tasks are unpinned,
allocate only at initialization and wait for worker exit before removing drivers.
The application serializes start/stop calls.

Queued non-Stop events older than 50 ms expire; Start/Stop replace pending
backlog. Observed disconnect/suspend or shutdown clears that port's queue. A full
queue rejects new events; a full USB endpoint counts a failed write rather than
replaying delayed clocks. These are reported losses, not a claim of lossless
clocking. Each port exposes ready/pending/accepted/sent/dropped/expired/failed
counters, and submission returns the mask actually accepted. Events already
accepted by TinyUSB belong to its FIFO/endpoint; clearing the application queue
cannot recall them. Host stalls/suspend may delay those accepted packets, which
must be characterized before accepting end-to-end clock output. Physical timing and
fault behavior remain HV-014 gates.

References: [ESP-IDF UART driver](https://docs.espressif.com/projects/esp-idf/en/v5.5/esp32p4/api-reference/peripherals/uart.html)
and the pinned TinyUSB MIDI class in `firmware/esp32/managed_components/`.


## USB MIDI port roles (2026-09-23)

Settings → MIDI offers **USB port mode: Device / Host**. Select with the
encoder or **Device / Host** softkey, press **Save mode**, wait for **Saved —
restart to apply**, then restart WaveX. Active mode, saved mode, connection
and save result are separate readbacks. Leaving without saving discards the
draft. Device remains the default when no setting exists; invalid/unreadable
NVS also boots Device and reports the settings error. NVS is never erased
automatically, and failed writes preserve the last confirmed saved choice.
This is a device setting, independent of Track routing and Project persistence.

Device mode keeps the existing TinyUSB computer/DAW connection. Host mode
uses ESP-IDF's host library on the same HS OTG controller, with one directly
connected class-compliant **USB MIDI 1.0** input adapter. The first eligible
alternate-zero MIDIStreaming interface and cable zero are used; additional
cables, devices/hubs, vendor-specific drivers and MIDI 2.0 UMP are not supported.
An input-only adapter is accepted. If the interface also supplies an OUT
endpoint, Sequencer USB clock/Start/Continue/Stop/SPP output uses it. Musical
note/CC output and MIDI THRU remain outside this change.

`usb_midi_port.cpp` owns the synchronized role/settings snapshot. The main
application loop services the queued NVS save, without holding the UI lock.
The USB host worker alone owns its device/interface and two reusable IDF
transfer allocations. Callbacks only retire transfers and mark received data;
parsing/forwarding runs outside callbacks. It validates descriptor lengths,
MIDI version, endpoint shapes and event packet sizes, skips other cables,
and uses the existing MIDI parsers and inter-MCU protocol. Up to one 512-byte
RX transfer and one output packet are serviced per tick. Transfer errors close
the session and require reconnection; no failed clock packet is replayed.
Disconnect/fault cleanup stops output, flushes endpoints, releases admitted
held notes in bounded groups through the existing note-release retry service,
and waits for all DMA completions before releasing the interface. Buffers are
allocated once per boot, never per MIDI event or reconnection.

The connector adapter must carry data and provide suitable 5 V VBUS power to
the attached MIDI device. Firmware host mode alone does not provide a power
supply or change board wiring; this implementation adds no VBUS GPIO control.
Check the board revision/cable power path before the host bench test. Device
and Host are mutually exclusive on this port; the independent USB Serial/JTAG
console remains available. Switch back to Device and restart before connecting
the OTG port to a computer.

Validation: eleven production-driver/parser/settings tests pass under ASan/UBSan,
including truncated descriptors, input-only adapters, cable isolation, DMA
cancellation, held-note cleanup, clock completion and NVS failure. Hardware
support, rendering, power, latency and jitter are **unverified**; see
[HV-035](../hardware-validation.md#hv-035--usb-midi-host-and-port-mode).

References: [ESP-IDF 5.5 USB host API](https://docs.espressif.com/projects/esp-idf/en/v5.5/esp32p4/api-reference/peripherals/usb_host.html),
[USB MIDI 1.0 class specification](https://www.usb.org/sites/default/files/midi10.pdf).
