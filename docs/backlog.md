# Backlog

Unscheduled work and open decisions. Promote an item to
[`roadmap.md`](roadmap.md) when it is required by a phase gate. Completed work
belongs in [`CHANGELOG.md`](../CHANGELOG.md) and git history, not here.

## Firmware audit remediation — 2026-09-06

This is the active task list for the audit of first-party
`firmware/esp32`, `firmware/daisy`, and `firmware/shared`. Vendor code is
checked at the API/DMA boundaries used by WaveX. Priorities describe concrete
failure modes; hardware-only gates stay in the roadmap. Remove each task when
its fix and regression checks are committed.

- [ ] **Medium — remaining reference consistency.** Finish checking the
  platform and feature guides against live transport and parameter behavior;
  the navigation, sequencer and testing references have been consolidated.
  Keep hardware-only results separate from host coverage (principles 13, 15).

## Performance, build, and transport

### Callback capacity checkpoint — 2026-09-07

The recurring callback gate is now measured in
[callback-performance-log.md](callback-performance-log.md). The WaveX 24 dB
path reached 65.8029% for 3606.0 seconds with zero underruns and remains in
the STAY band. The DaisySP comparison reached 89.6635% for 605.2 seconds with
zero underruns and is UPGRADE because callback-resident work remains. This
activates backend-upgrade planning in the Phase 2 capacity checkpoint and the
[RT1170 migration plan](rt1170-migration.md); it does not authorize a board
port or purchase.

Remaining optimization questions are to measure Render() separately, test
selective placement A/B, decide whether slope/drive/topology become real
parameters, and establish the effect of any future parameter-lock DSP. The
WaveX path remains the fallback until those questions and the migration work
are resolved.

### Page-entry render cost

Sample Edit, Sample Record, and Play have expensive first frames (up to about
205 ms for Sample Edit) despite inexpensive steady-state rendering. Profile a
smaller or filled waveform representation before changing the widget, and
spread page construction across frames only if rendering cannot meet the
interaction target.

### Daisy optimization and LTO

The Daisy image defaults to `-O2`. The recurring QSPI `-O2` evidence is in
[callback-performance-log.md](callback-performance-log.md): WaveX stays below
70%, while the DaisySP comparison activates the backend-upgrade planning
checkpoint. Consider LTO only after the remaining Render() and placement
measurements; it can affect linker section placement and weak HAL symbols.

### Profile-guided QSPI-to-SRAM execution

The persistent QSPI image already supports selective relocation:
`WAVEX_ITCM_CODE` gives code a QSPI load address and an ITCM run address, and
`MemorySections::InitItcm()` copies it before interrupts start. Currently only
the UART RX-position handler uses it. Profile the audio callback first, then
move a measured hot region such as `VoiceManager::Render()` only when the DWT
results justify the extra linker/startup complexity. Retaining its
host-testable header implementation is preferable to an unmeasured placement
change.

A matched `-O0`, profiling-enabled bench on 2026-09-04 streamed the same
44.1-kHz stereo WAV from SD in both profiles. The active audio callback
averaged 52.39 us from QSPI and 17.84 us from SRAM; the foreground WAV pump
averaged 1.416 ms and 1.189 ms respectively, with no observed underruns. The
current release-optimized eight-voice evidence is recorded in
[callback-performance-log.md](callback-performance-log.md); selective
relocation still needs a measured placement A/B before it is accepted.

If selective ITCM placement is insufficient, evaluate a separate persistent
bootloader-SRAM profile. libDaisy's `BOOT_SRAM` model stores the application in
QSPI and copies it into SRAM at boot, so runtime should resemble the direct-SWD
SRAM profile while surviving power cycles. Do not replace the existing QSPI
execution profile unless the image remains below the bootloader's 480 KiB
limit, D1 heap and the currently tight D2/D3 regions retain measured headroom,
and an identical eight-voice plus SD-streaming soak shows a worthwhile DWT
improvement with zero underruns. Record flash time, boot-to-audio latency, map
usage, and callback min/average/max for both profiles.

### SPI-link revival requires hardware verification

UART remains the live transport. The September 2026 SPI audit fixes descriptor
ownership, duplex RX publication, parser capacity, sequence/length handling,
READY signaling and bounded recovery in the compiled-out adapters. It does not
enable SPI or change the Phase 2 transport decision.

The remaining gate is startup/routing integration, a measured READY/CS timing
proof, bidirectional DMA under audio/SD load, fault injection and peer-reboot
recovery. Define application retry behavior for CRC rejection and a peer that
never reasserts READY. The Daisy recovery path requires exclusive ownership of
libDaisy's SPI DMA streams; resolve Stage B CV sharing before combining them.

Use [spi-notes.md](spi-notes.md#verification-and-remaining-gates) for evidence,
limits and the bench gate. The GPIO continuity test passed; high-speed signal
integrity and the corrected DMA path have not been verified on hardware.

### Streaming CRC recovery

SD CRC recovery can block the main loop longer than the audio ring lasts.
Choose and implement bounded recovery behavior: pause and recover, abort the
stream, or increase the prebuffer. Validate the choice with injected or
reproducible CRC faults and capture ring low-water and service latency.

### Daisy build warnings worth acting on (2026-09-06)

The release build is not warning-clean, and two of the warnings are real:

- `PumpSampleMetaPage`: the page's byte count is a `size_t` narrowed to
  `UartLinkSend`'s `uint16_t` (`-Wconversion`). In range today (one header
  plus at most a page of records); make the narrowing explicit with a bound
  so a wider record cannot wrap it.
- `sfz_loader.cpp`: `FF_USE_LFN` tested with `#if` where it is not defined
  (`-Wundef`, twice) and an enum/int mix in a conditional (`-Wextra`).

### The SRAM debug profile has ~7 KB of RAM_D2 headroom

`make daisy-debug` / `make flash-fast` link the backend to run from SRAM with
its data in `RAM_D2` (256 KB). At `eb15470` (the path widening) that link
failed — `RAM_D2` overflowed by 9532 bytes — so the fast bench loop could not
load the Daisy at all and only the DFU path (`make daisy-flash-auto`) worked.
Retiring the decimated preview (`6f53720`) freed ~16 KB and it links again at
97.4% (255 276 B), which leaves about 6.9 KB. The next resident buffer of
that size breaks the debug profile before it troubles the release one; the
`.wxi` document buffer move in [the SRAM item](#the-daisys-sram-is-at-89-and-the-wxi-document-buffer-is-the-lever)
is the lever for both. `make flash-fast` should report the Daisy link
failure by name — today it prints the ESP32's success and exits 1.

## Memory

### The Daisy's SRAM is at 89%, and the .wxi document buffer is the lever

The 2026-09-06 path widening took internal SRAM from ~84% to 89.1%
(467 KB of 512 KB, ~57 KB free); retiring the decimated preview the same day
(protocol 3) gave ~16 KB of that back (its 4096-point buffer and frame
staging), for 86.0%. The single largest new consumer is the loader's `.wxi`
document buffer (`s_doc_storage`, `sfz_loader.cpp`): a `Wxi::InstrumentFile`
is ~17 KB once each of its 64 zone slots carries a 256-byte path.

It does not belong in SRAM. It is a main-loop working buffer, written once per
load and never touched by the audio callback — the same profile as the Sample
Pool's records, which already live in SDRAM. Moving it recovers ~17 KB.

**Do not reach for `__attribute__((section(".sdram_bss")))` to do it.** That
section exists in both linker scripts and starts at the SDRAM origin, which is
exactly where `sdram_layout.h` puts the *sample arena* (`kBase = 0xC0000000`).
The layout reserves nothing for linker-placed SDRAM data, so anything landing
there silently overlaps user sample memory. Carve the buffer from a partition
the layout owns — the render scratch is unused and adjacent — or extend the
layout to reserve a linker-placed region first.

When to revisit: before the next feature that adds a large resident buffer, or
if a build reports SRAM above ~92%.

## Testing

## UI and frontend maintenance

### Touch coordinate verification

The GT911 and rotated 1280×720 LVGL canvas may use different coordinate
orientations. Corner-tap the actual panel before changing driver configuration;
do not guess an axis swap in the vendored BSP.

### Break the `components/ui` ⇄ `main` dependency cycle

Pages call frontend globals directly, so the UI component cannot be host-tested
independently. Inject a narrow `UISharedContext` through the navigator, move
`ICommInterface` out of `main`, then remove the `main` CMake requirement. Do
this after the current UI hardware pass, not alongside unverified behavior.

### Busy-overlay errors

Errors currently use an in-flight spinner and can be rewritten by its timeout.
Add a non-spinning `showError()` path with stable text and an intentional
dismissal policy.

### Encoder counter race

The PCNT read-then-clear sequence can lose counts. The current re-centering
makes it rare; close the race only with an IRAM-safe watch-point design or by
moving consumption into the UI task.

### Unused frontend code

In a separate deletion pass, remove the caller-less ESP32 APIs, unused window
manager, and unreachable UI surfaces after re-checking callers.

## Samples, instruments, and browsing

### Non-frame-aligned WAV data

Some legal WAV files whose `data` payload begins off a frame boundary produce
artifacts. Reproduce with a minimal fixture and identify the parser or stream
path fault; do not round the offset to a frame boundary without proof.

### Resident-sample status capacity

The status message and ESP32 metadata cache cover eight entries while the
Daisy can hold 32. Before kits or multisampled instruments depend on it, add
paged status or an explicit sample-unloaded message so the frontend can
reliably remove stale rows.

### Browse paging

Directory listings stop at 256 entries. Implement paging or a bounded cache
when the 500-entry browse target becomes a product requirement; increasing a
static array is not the solution.

### Sample operations

There is no protocol operation to unload, rename, delete, or reorder a loaded
sample. Define one versioned `MSG_SAMPLE_OP` verb in a reserved protocol block,
with round-trip tests, after its effects on instrument references are defined.

### Wavetable oscillator source

The architecture now reserves a typed oscillator-source boundary so sampler and
wavetable engines can share Instrument/Track/Voice ownership without pretending
that a wavetable is a short looping sample. Its place is fixed: one of an
Instrument's two `Oscillator` slots, `OscType::Wavetable`, with `WT_POS` as its
mod destination ([track-and-patch-model.md](features/track-and-patch-model.md)
§3.1). The wavetable renderer is an unscheduled, post-Phase-2.5 candidate;
sampler reliability, Instrument persistence, sequencing, and the zero-underrun
gates come first.

Before promotion to the roadmap, resolve the implementation decisions listed in
[oscillator-sources.md](features/oscillator-sources.md): import metadata,
cycle/frame limits, interpolation and anti-aliasing, modulation behavior, WXCF
chunks, RAM admission, and a measured DWT budget. Any earlier refactor may only
extract the typed source seam as a small, behavior-preserving change needed by
scheduled sampler work.

### Oscillator sync and FM

Requested 2026-09-12. Unscheduled, post-Phase-2.5 candidate for an
Instrument-owned interaction between its two oscillator slots. Before any
implementation, decide master/slave direction; hard versus soft sync for
sample and loop sources; true-frequency versus phase modulation; routing,
depth and feedback policy; alias suppression; source compatibility; saved
settings; and the measured callback budget. Sampler reliability, persistence
and current audio gates come first. This item does not define a protocol or
enable audio behavior.

### Parameter-id design drift

The target parameter-lock document reserves IDs that conflict with live pan and
pitch values. Reconcile the design with `protocol.h` before implementing
parameter locks or modulation; do not renumber live wire values without a
protocol migration.

## Hardware, diagnostics, and logging

### ESP32 pin verification

`pin_config.h` was reconciled against the ESP32-P4-WIFI6 header on 2026-09-05
(the SPI2/PCNT collision is gone; see `features/panel-controls.md`). What is
left is bench work, tracked in `roadmap.md` § Outstanding hardware
verification: which encoder is physically wired and to what, and the keypad
matrix geometry. The dormant SPI-slave link's five pins stay reserved until
the SPI revival decision above is made; releasing them for the panel is the
alternative if that decision is "never".

### Logging policy

Fold legacy `UART_LOGx` and Daisy compatibility call sites into the module
table incrementally. Decide the release log ceiling from field-diagnostics
needs, not the roughly 4 KB text saving at WARN alone.

### Backend link instrumentation

`WAVEX_DAISY_UART_PERF_DEBUG` gates backend link counters. Measure its
hot-path cost before deciding whether cheap counts should remain enabled in
release and whether the flag follows the build profile.

### Filter: promote slope, drive and topology to real parameters

`audio/voice_filter.hpp` (2026-09-04) makes the per-voice lowpass selectable
- WaveX TPT SVF at 12 or 24 dB/oct with a soft-clip drive, or `daisysp::Svf`
- but only through the debug console (`WAVEX-FILTER`), for A/B listening.
Whatever the listening decides:

- **Slope and drive** belong on the Instrument (they are voice character,
  like resonance): a `PARAM_FILTER_SLOPE` / `PARAM_FILTER_DRIVE` pair in
  `protocol.h` with round-trip tests, a Zone/Instrument field with SFZ
  defaults, and Voice-page controls. Until then both default off, so the
  filter is the linear 12 dB one it always was.
- **Topology** is a build-time decision once the comparison is done: keep
  one, delete the other and `VoiceFilter`, or keep both behind a per-Instrument
  field if they turn out to be complementary voices rather than a better and a
  worse one. The DaisySP side costs ~1.8 KB of flash and ~3x the per-sample
  CPU of the 12 dB WaveX stage.
- Whichever wins, DWT-measure `Render()` at eight voices with drive on and
  24 dB before calling it done (roadmap "Callback budget").

### Daisy image size: what is left after the 2026-09-04 slimming

The Daisy image went from 438 KB to 273 KB. Remaining, in order of size, each
a decision rather than a mechanical fix:

- ~~`WAVEX_DAISY_OPT` is still `-O0`~~ Done 2026-09-04: `-O2` by default
  (image 275 772 to 189 636 bytes). The whole-callback `-O2` soak is in
  [callback-performance-log.md](callback-performance-log.md); `Render()`
  timing and `-O3` still need a reasoned comparison.
- **USB CDC logging (`hw.StartLog`) is in every profile**, ~19 KB flash and
  13 KB SRAM including `stm32h7xx_ll_usb.c`. Gate it on `WAVEX_BUILD_DEBUG`
  once field logging is confirmed to go via the UART bridge; libDaisy's
  `usbd_core.c` also carries the only remaining `printf` callers (now routed
  to the log ring).
- ~~Stack temporaries of the `obj = T{}` form~~ and ~~`-Wformat-truncation`
  at `-Os`~~ both done 2026-09-04 (75145f3).
- **`HAL_HCD_IRQHandler` + `hhcd_USB_OTG_HS` (~2 KB)** stay linked through
  libDaisy's own `system.cpp` IRQ table; removable only by a submodule change.

### Diagnostics coverage

Add active-voice and round-trip-latency telemetry when it has an owning
protocol change. Add MIDI detail and dropped-event counters with the Phase 2
sequencer and tempo-follower work, when those events actually exist.

The Link tab's `unknown` count is meant to read 0 in a healthy session, so a
non-zero value means corruption or a protocol mismatch. It does not yet: the
backend answers every audition-by-path with a `MSG_ACK` (`serial_id` 0,
`ProcessSamplePlayRequest`) that the frontend neither routes nor counts as
known, so each audition adds one. Either drop that ACK - nothing waits for
it - or route it; do not just add it to the known list, which would hide a
message the frontend does nothing with.

### Root-menu context lines have no data source

The refreshed root menu (design turn 2e) gives every row a live context
readout saying what it currently points at. Only two could be wired
truthfully: Play shows the selected Track, Diagnostics shows link health from
the backend heartbeat. Two more are specified by the design and are blank:

- **Sample — resident count.** No API exposes how many samples are in the
  backend's sample RAM. The number exists in `SampleMemStatusMessage`, which
  `ui_diagnostics_page.cpp` decodes into its own widgets and does not publish.
- **Instrument — instrument name.** `instrument_name_` is a private member of
  `UIInstrumentPage`, so nothing outside that page can read it.

Both want a small shared accessor of the same shape as `current_track.h` /
`current_sample.h` rather than a second copy of the state. Until then the rows
show no context, which is the honest rendering - a placeholder in the root
menu would have to be opened to find out whether to believe it.

### mocks/ui_theme.h is dead

Nine UI sources include the theme as `"../styles/ui_theme.h"`, which bypasses
`firmware/esp32/tests/mocks/ui_theme.h` entirely - the host test build
compiles the real header and always has. The mock is stale and unused;
either delete it or change those includes to `"ui_theme.h"` so the mock is
actually what the host build sees. Left alone for now because changing it
mid-redesign would swap the palette the host build compiles against.

## Related

- [Roadmap](roadmap.md) — phased work and hardware verification gates.
- [Architecture](architecture.md) — target design and real-time constraints.
- [Inter-MCU protocol](features/inter-mcu-protocol.md) — message ownership and
  reserved ranges.
