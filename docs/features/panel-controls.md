# Panel Controls — Buttons, LEDs, Endless Pots, and MIDI I/O

**Status**: Design, 2026-09-05. Nothing below is built except where §1 says
so. Pin numbers live only in `firmware/shared/config/pin_config.h`; feature
flags and table sizes only in `hardware_config.h`. This document names the
functions those pins carry and the rules that produced the allocation — never
the numbers.

**Where it fits**: Phase 2 of `roadmap.md`. The Phase 2 gate is "program and
perform a four-track pattern with swing *from the panel*", item 2 needs DIN
MIDI pins that do not collide with the flash port, and item 3 (pad grid, LED
feedback) needs the drivers and the input model designed here. It is therefore
a Phase 2 prerequisite, sequenced before items 2 and 3 as "2.P".

---

## 1. Where we start (as-built, 2026-09-05)

What the ESP32 image actually does with the panel today, from code (file
references are the audit trail; re-verify before trusting):

| Piece | State |
|---|---|
| Touch (GT911) | The only `lv_indev` fed by hardware. Bus is the BSP's I2C (`bsp_i2c_get_handle()`), shared with the keypad. |
| Encoders | Two PCNT units, 4x quadrature decode, glitch filter, polled at 2 ms (`main/pcnt_task.cpp`). Unit 0 posts raw counts; unit 1 divides by the detent constant. Both reach pages as `InputEvent`s. The bench encoder is unit 1 (confirmed 2026-09-05); unit 0 has nothing wired, and its channel B had pointed at a GPIO that is not on the board's header. Unit 1's phases were swapped in config so clockwise counted negative — fixed 2026-09-05 (`pin_config.h`), together with the three pages that had compensated for it. No encoder push handler exists; "encoder click" is TCA8418 keycode 3. |
| Keypad (TCA8418) | Driver present and started (`components/ui/src/tca8418_keypad.cpp`), polled at 10 ms, INT pin configured but not used. **Only four keycodes are mapped** (Select, Back, EncoderClick, Shift); every other key is dropped. Matrix geometry (`WAVEX_TCA8418_ROWS/COLUMNS`) has never been verified against the wiring. |
| Softkeys | Six on-screen buttons, touch only. `SoftkeyBar::focusNext()` / `pressFocused()` exist with no callers — the documented "encoder scrolls softkeys" interaction is not in the binary. |
| Shift | Latched-and-sticky global modifier in `InputDispatcher::processAll()` with a header chip. Works, driven by keycode 4 today. |
| LEDs (TLC5947), pot ADC (MCP3008) | **No driver, no SPI2 bus init, nothing.** Only config constants. |
| DIN MIDI | Compiled out since 2026-09-04: RX sat on the USB-Serial/JTAG D- pin. Pins moved 2026-09-05; still off until rewired (§5). TX ring is zero-length — no MIDI out. |
| USB MIDI | Device on the **USB 2.0 High-Speed OTG** controller (`TINYUSB_DEFAULT_CONFIG()` selects the HS port on the P4), i.e. the board's 4-pin USB connector, independent of the flash port. Input only; `WAVEX_USB_MIDI_OUTPUT_ENABLED` is read by nothing. |
| Input plumbing | `InputEvent` → `InputDispatcher` queue (64 deep) → drained on the UI task under the LVGL lock → global Shift/Back → `UIPage::onInput()`. The debug console injects the same events (`KEY`, `ENC`, `POT`). |

The good news is that everything downstream of `InputDispatcher::post()` is
already generic. The panel work is (a) drivers, (b) a real logical-key and
LED namespace, and (c) the page contract for pots — not a UI rewrite.

## 2. Parts: reassessment

| Part | Verdict | Why |
|---|---|---|
| **TCA8418** keypad controller (I2C) | **Keep.** | Up to 80 keys on two wires plus INT, hardware debounce, 10-event FIFO; already on the touch I2C bus and already driven. Every panel key, both encoder push switches and the 16 Phase-2 pads fit in one part with rows to spare. Two caveats: the vendored driver hard-codes 100 kHz for its device (fine — events are tiny), and it cannot do velocity. Pads are on/off switches in this design; velocity comes from touch position or a fixed level, as `sequencer.md` §5 already says. |
| **TLC5947** 24-ch 12-bit constant-current LED driver | **Keep, two chained (48 ch).** | Sinks up to 30 mA per channel with one IREF resistor, 12-bit PWM so dim states read as dim, chainable, three signals beyond the shared SPI clock/data. It has no chip select — it is a shift register — which is the one rule the SPI2 driver has to respect (§3.3). If per-pad **RGB** is ever wanted, 16 pads alone need 48 channels; switch to an I2C matrix driver (IS31FL37xx class) then rather than chaining four TLC5947s. Not a v1 concern. |
| **MCP3008** 8-ch 10-bit SPI ADC | **Keep.** | Eight channels is exactly four endless pots. 10 bits over a wiper's ~180° linear span is ~0.2°/count before noise, more than the UI can use. If finer control is ever wanted the **MCP3208** is the same footprint and protocol with 12 bits — a one-constant change (`WAVEX_POT_ADC_RESOLUTION`). The chip's own on-board ADC was considered and rejected: the P4's ADC-capable header pins are all spoken for (I2C, inter-MCU UART, the SPI-slave reserve), and it would cost eight GPIO where the MCP3008 costs four. |
| **PEC11R** detented quadrature encoders (x2, PCNT) | **Keep both** as navigation encoders. | Already working through the hardware pulse counter, glitch-filtered, no CPU cost. Detents suit list navigation and value stepping; the push switch gives Select. The endless pots are a different tool (§2.1). |
| **CD74HC4067** analog mux | **Dropped** (removed from `hardware_config.h` 2026-09-05). | Predated the MCP3008; its plan used the chip's single ADC through a mux on address pins that are not on this board's header. |
| Endless pots ("dual pots at 90°") | **Adopt, four.** Example part: Alpha RV112FF-40B1 series (two wipers 90° apart, 360° endless). Confirm the exact part before the panel PCB; the decoder (§4.4) is written against the two-wiper triangle-wave family, not one vendor. | See §2.1. |

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
   revival decision (`backlog.md`) is made. They cost the panel nothing it
   needs today; releasing them is a one-line decision later.
4. **Adjacent header pins for each pair** (encoder A/B, MIDI RX/TX, the SPI2
   run) so each control is one small connector.

Under those rules the panel needs nine pins and gets them, with **one spare**
(earmarked for a second MCP3008 chip select if more than four pots or any
plain pots are ever added). The result is in `pin_config.h`, with the
reasoning for each move recorded there.

### 3.2 Bus topology and ownership

```
ESP32-P4
├─ I2C (BSP bus, 400 kHz)      GT911 touch ── TCA8418 keypad (100 kHz device clock) + INT
├─ SPI2 master                 TLC5947 #0 ─ TLC5947 #1 (chain; XLAT, BLANK)   MCP3008 (CS)
├─ PCNT unit 0, unit 1         NAV_A, NAV_B quadrature (push switches → TCA8418 matrix)
├─ UART1                       inter-MCU link (as-built)
├─ UART2                       DIN MIDI in/out
├─ USB 2.0 HS OTG (4-pin conn) USB MIDI device (as-built) — to the DAW
├─ USB-Serial/JTAG (Type-C)    flash / debug — never touched by WaveX code
└─ USB 1.1 FS pair             reserved, unpopulated: future USB host
```

**One task owns SPI2.** The MCP3008 reads and the TLC5947 frames go through
the same bus from the same task (`panel_task`, §4.1), so there is no bus
mutex and no way for a pot read to interleave with a half-shifted LED frame.

### 3.3 The TLC5947 rule

The TLC5947 has no chip select. Every clock edge on SPI2 — including the
MCP3008's — shifts data through its 288-bit register. That is harmless only
because outputs change on **XLAT**, not on shift. Therefore:

- The driver always sends the **complete** 72-byte chain frame immediately
  before pulsing XLAT, never a partial update.
- BLANK is held high (all outputs off) from power-on until the first frame is
  latched — via a pull-up so the LEDs are dark before the firmware runs, then
  driven by the GPIO. BLANK also gives free global dimming/screen-blanker
  integration.
- SPI2 carries two `spi_device` handles with their own clocks (TLC5947 up to
  30 MHz, MCP3008 ~2 MHz at 3.3 V); ESP-IDF serialises them on the bus.

### 3.4 Control inventory (v1)

Keys (all TCA8418 matrix; the count below fits comfortably inside the part's
maximum — `WAVEX_TCA8418_ROWS/COLUMNS` follow the panel PCB, not this table):

| Group | Keys | LED |
|---|---|---|
| Softkeys | `SOFT1`…`SOFT6`, directly under the screen's six softkey buttons | one each (dim = defined, bright = latched/active state) |
| Modifier | `SHIFT` | one (mirrors the header chip) |
| Menu jumps | `SAMPLE`, `PLAY`, `INSTRUMENT`, `TRACK`, `MIXER`, `SETTINGS` — the root groups of `ui-information-architecture.md` §1 | one each (lit = the active root group) |
| Navigation | `BACK`, `NAV_A_PUSH` (Select), `NAV_B_PUSH` | — |
| Track | `TRACK_PREV`, `TRACK_NEXT` | — |
| Transport | `PLAY_STOP`, `REC` | one each (Phase 2 semantics) |
| Pads | `PAD1`…`PAD16` (Phase 2 pad grid; wire now, semantics later) | one each |

That is 37 keys and 31 LEDs, inside `WAVEX_LED_CHANNELS` with room spare.
`hardware_config.h` sizes (`WAVEX_LED_CHANNELS`, rows/columns) follow the
final panel PCB, not this table.

Analog: four endless pots, each two wipers → eight MCP3008 channels. Wipers
run wiper-to-wiper between 3.3 V and ground with the MCP3008 on the same
3.3 V reference so calibration is a min/max per channel, not a ratio.

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
panel_task (new, prio 5, 2 ms)        keypad_task (existing → INT-driven)
├─ PCNT unit 0/1 deltas               └─ TCA8418 FIFO on INT, 100 ms fallback poll
├─ MCP3008 8-ch burst read                 │
├─ endless-pot decoder → deltas            │  physical keycode → PanelKey (table)
├─ LED frame flush if dirty                │
│      │  InputEvent (Pot/Encoder)         │  InputEvent (Key press/release)
│      └──────────────► InputDispatcher::post() ◄──────────┘
│                               │
│              UI task: InputDispatcher::processAll()
│              ├─ global keys: SHIFT, BACK, SOFTn, menu jumps, TRACK±, transport
│              └─ page: UIPage::onInput() / pot bindings
│
└─ ◄── LED frame (double-buffered, dirty flag) ◄── PanelLeds (UI-task-owned model)
```

- `panel_task` absorbs today's `pcnt_task`: one 2 ms poller instead of two,
  and it is the **sole SPI2 user**. It never touches LVGL.
- The keypad task switches to the INT line (the TCA8418 `CFG` register's
  `KE_IEN` bit is written, INT falling edge → task notification), keeping a
  slow poll as a safety net. Latency drops from ≤10 ms to sub-millisecond,
  which matters once pads and transport keys exist.
- The UI task owns all *meaning*: which key does what, what the LEDs show.
  It publishes the LED frame into a double buffer; `panel_task` flushes it.
  The UI task never blocks on SPI.

### 4.2 Data model

Entities and where their truth lives:

| Entity | Shape | Source of truth |
|---|---|---|
| `PanelKey` | `enum class : uint8_t` — the logical keys of §3.4, replacing the four `BUTTON_*` constants in `ui_softkey.h` (kept as aliases during the transition) | `components/ui/include/ui/panel_key.h` |
| Key map | TCA8418 keycode (row·10 + col + 1) → `PanelKey`, one table, `static_assert` no duplicate keycode | `hardware_config.h` (it is wiring truth, like a pin) |
| `PanelLed` | `enum class : uint8_t` — the LEDs of §3.4 | `components/ui/include/ui/panel_led.h` |
| LED map | `PanelLed` → TLC5947 channel index, one table, `static_assert` no duplicate channel and all `< WAVEX_LED_CHANNELS` | `hardware_config.h` |
| `InputEvent` | Existing struct gains `InputType::KeyPress/KeyRelease` carrying a `PanelKey` in `source_id`, and `InputType::PotUp/PotDown` carrying the pot index in `source_id` and the magnitude in `delta` — same magnitude-plus-direction contract `steps()` already enforces. Existing `Encoder*` types stay for the nav encoders. | `input_event.h` |
| `EncoderBinding` | `{ const char* label; const char* value; void (*onSteps)(int); }`, four per page, mirroring `Softkey` | `ui_page.h` |
| `PanelLeds` | UI-task-owned array of `uint16_t` levels indexed by `PanelLed`; `set()`, `flushIfDirty()` | `components/ui/src/panel_leds.cpp` |
| Pot calibration | per-channel min/max + per-pot direction, persisted in NVS (`wavex/panel`), defaults from the datasheet range | `main/panel/endless_pot_store.cpp` |

Invariants: every physical keycode maps to at most one `PanelKey`; every
`PanelLed` maps to exactly one channel; `SHIFT` and `BACK` never reach a
page (already true); a page's four bindings are re-read on every
`refreshSoftkeys()` so labels follow state exactly as softkeys do.

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

Pot deltas go to the page's `EncoderBinding[n].onSteps(steps)`; Shift +
pot divides the step size (fine mode) in the dispatcher, not per page.

### 4.4 Endless-pot decoding (host-testable)

`firmware/shared/panel/endless_pot.hpp`, HAL-free, C++17, under
`firmware/shared/tests/`:

1. Normalise both wipers to [0,1) with the per-channel calibration.
2. Each wiper is a triangle wave over the turn; at any angle one of the two
   is in its linear region (away from its fold). Pick that wiper, use the
   other's sign to disambiguate the slope, and reconstruct the angle.
3. Delta = wrapped difference from the previous angle; a dead-band (about
   two ADC counts, configurable) suppresses noise; an optional acceleration
   curve maps angular speed to steps. Output is signed steps per 2 ms sample.
4. A stuck or disconnected wiper (both channels at rails) reports "no pot"
   rather than spinning; the Diagnostics tab shows it.

Tests drive synthetic sin/tri wiper pairs forward and backward at several
speeds with added noise and assert monotonic step output, zero drift over a
full turn, and no steps below the dead-band.

### 4.5 LED policy (v1)

Driven entirely from navigator/page state — no page sets an LED directly:

- `SHIFT` LED = `isShifted()`.
- Jump LEDs: the active root group bright, others off.
- Softkey LEDs: defined softkey dim, undefined off; a page may mark one
  softkey `active` (e.g. "Stop" while auditioning) → bright. This needs one
  `bool active` on `Softkey`.
- Transport/pad LEDs: Phase 2 (`sequencer.md` §5: step/playhead mirror).
- Screen blanker: `DisplayManager` blank → BLANK high (all LEDs off); any
  panel input wakes both.

### 4.6 Diagnostics, console, tests

- **Diagnostics → Panel tab**: last keycode with row/col and its `PanelKey`
  (or "unmapped"), raw MCP3008 values and decoded angles per pot, PCNT
  counts, an LED walk test (Select cycles channels) and a full-on test. This
  is how the matrix geometry and LED map get *verified* rather than assumed.
- **Console**: `KEY <PanelKey name> [PRESS|RELEASE|TAP]` (every logical key
  by name — today it knows four), `POT <n> <delta>`, `LEDS` in `STATE` (the
  48 levels), `PANEL` (raw ADC + pcnt snapshot). `make test-hil` then covers
  menu jumps, softkey keys honouring Shift, pots reaching a page binding,
  and LED state following navigation — with no camera.
- **Host tests**: endless-pot decoder (§4.4); key-map and LED-map table
  uniqueness are `static_assert`s and need no runtime test.

### 4.7 MIDI software

- DIN: re-enable `WAVEX_ESP_DIN_MIDI_ENABLED` once the receiver is on the
  new RX pin; keep the storm detector. Give UART2 a TX ring and a
  `midi_out_send()` used by both DIN and USB — Phase 2 item 2 (clock out) is
  a consumer of this, not a place to grow it.
- USB: implement the output half (`tud_midi_stream_write`), which
  `WAVEX_USB_MIDI_OUTPUT_ENABLED` already claims. Bench-confirm the device
  enumerates on the 4-pin HS connector, not the Type-C.
- Both inputs already funnel into `midi_forward_event()`; unchanged.

## 5. Stages (each one commit, each independently buildable)

| # | Stage | Host-verifiable | Bench |
|---|---|---|---|
| 0 | **Pin reconciliation** — `pin_config.h` rewritten against the WIFI6 header; CD74HC4067 removed; MIDI pins moved; encoder phase order corrected. *Done 2026-09-05.* | compiles | clockwise is forward on every page |
| 1 | **`PanelKey` / `PanelLed` model + key map** — enum, table in `hardware_config.h`, `InputEvent` extensions, `KEY <name>` console verb, dispatcher handling for `SOFTn`, jumps, `TRACK±`; `SoftkeyBar::press(n)`; `UINavigator::jumpToRoot()`. Delete the dead `focusNext/pressFocused` or wire them — not both. | HIL: jumps, softkeys via key, Shift row | keycode → key on the Diagnostics tab |
| 2 | **Keypad INT** — `CFG.KE_IEN`, ISR → notification, fallback poll. | — | latency, no missed keys under a 10-key roll |
| 3 | **`panel_task` + SPI2 + TLC5947** — absorb `pcnt_task`; LED frame, BLANK, `PanelLeds`, LED policy §4.5, `LEDS` in `STATE`. | HIL: LED state follows navigation | walk test, dark at power-on, no flicker with pot reads |
| 4 | **MCP3008 + endless pots** — decoder (host tests), calibration store, Settings → Calibrate flow, `EncoderBinding` page contract, strip widget, Shift = fine. First consumers: Instrument page (Filter/Amp), Play page live strip. | decoder tests; HIL `POT n` | feel, drift, noise floor; measure the strip's cost on the 30 FPS budget |
| 5 | **MIDI** — DIN on, TX ring, USB out, latency measured (closes the roadmap's "MIDI latency" row). | — | DIN in → sound, USB in → sound, both < 5 ms |

**Gate**: from the panel alone (no touch), jump to Instrument, change the
filter cutoff on a pot and hear it, latch Shift and fire a shifted softkey,
return with BACK; all corresponding LEDs correct throughout; DIN and USB MIDI
notes sound; `make test` and `make test-hil` green.

## 6. Decisions taken here (revisit only with a reason)

1. Both encoder kinds: two detented nav encoders on PCNT, four endless pots
   on an MCP3008 (§2.1).
2. The second full-speed USB pair is reserved for a future host port; the
   dormant SPI-slave pins stay reserved (§3.1). Together that is seven GPIO
   held back, leaving one spare after the panel.
3. USB MIDI stays on the HS OTG port (already true) — the 4-pin connector is
   *the* USB MIDI port; the Type-C is only ever flash/debug.
4. Key and LED maps live in `hardware_config.h` alongside the other wiring
   truth rather than in a third config file.
5. SPI2 has one owner (`panel_task`); the UI task never performs bus I/O.
6. Pads are switches; velocity is not a panel-hardware feature in v1.

## 7. Out of scope / later

RGB pads; velocity-sensing pads; USB host (needs the reserved FS pair, a
`usb_host` stack and a power switch — Phase 5); MIDI THRU (hardware only, if
the panel PCB has room); moving MIDI DIN out to the Daisy if clock jitter over
the link proves too high (`sequencer.md` §1 already reserves that option).

## 8. Open hardware questions for the bench

Recorded in `roadmap.md` § Outstanding hardware verification:

- ~~Which encoder is physically wired, and to what?~~ Unit 1, confirmed 2026-09-05;
  direction fixed at the source. Remaining: confirm clockwise is forward on
  every page after the flash.
- TCA8418 matrix geometry (`WAVEX_TCA8418_ROWS/COLUMNS`, unconfirmed) — the Diagnostics
  Panel tab (stage 1) answers it.
- USB MIDI enumerates on the 4-pin HS connector — never confirmed on the
  bench (roadmap "MIDI latency" row).
- Endless-pot part and its wiper waveform (triangle vs sinusoid) — the
  decoder is written for triangle; verify on a scope before calibrating.
