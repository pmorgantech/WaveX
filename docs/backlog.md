# Backlog

Unscheduled work and open decisions. Promote an item to
[`roadmap.md`](roadmap.md) when it is required by a phase gate. Completed work
belongs in [`CHANGELOG.md`](../CHANGELOG.md) and git history, not here.

## Performance, build, and transport

### Page-entry render cost

Sample Edit, Sample Record, and Play have expensive first frames (up to about
205 ms for Sample Edit) despite inexpensive steady-state rendering. Profile a
smaller or filled waveform representation before changing the widget, and
spread page construction across frames only if rendering cannot meet the
interaction target.

### Daisy optimization and LTO

The Daisy image defaults to `-O0`. Establish a DWT baseline at 480 MHz, then
compare an `-O2` image under eight-voice audio and SD soak tests before choosing
a default. Consider LTO only after that decision; it can affect linker section
placement and weak HAL symbols.

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
averaged 1.416 ms and 1.189 ms respectively, with no observed underruns. This
supports evaluating selective relocation, but it is not a release-optimized
profile or the required eight-voice soak.

If selective ITCM placement is insufficient, evaluate a separate persistent
bootloader-SRAM profile. libDaisy's `BOOT_SRAM` model stores the application in
QSPI and copies it into SRAM at boot, so runtime should resemble the direct-SWD
SRAM profile while surviving power cycles. Do not replace the existing QSPI
execution profile unless the image remains below the bootloader's 480 KiB
limit, D1 heap and the currently tight D2/D3 regions retain measured headroom,
and an identical eight-voice plus SD-streaming soak shows a worthwhile DWT
improvement with zero underruns. Record flash time, boot-to-audio latency, map
usage, and callback min/average/max for both profiles.

### SPI-link revival is gated on six recorded defects

UART remains the live transport. Before enabling `WAVEX_SPI_LINK_ENABLED`, fix
and hardware-verify these defects in `esp_spi_link.cpp`:

- driver-owned transaction descriptors and RX buffers are reused after timeout;
- the RX path has no sequence gate;
- an uninitialized capacity can overflow a 220-byte stack buffer;
- the TX sequence wraps through reserved value zero;
- configured and actual transfer lengths are confused; and
- the slave ISR callback has not been audited for IRAM safety.

Use [`spi-notes.md`](spi-notes.md) for the DMA/slave bring-up gate. Keep UART
as the fallback until the link is bench-proven.

### Streaming CRC recovery

SD CRC recovery can block the main loop longer than the audio ring lasts.
Choose and implement bounded recovery behavior: pause and recover, abort the
stream, or increase the prebuffer. Validate the choice with injected or
reproducible CRC faults and capture ring low-water and service latency.

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

### Softkey allocation and unused frontend code

Softkey deferral allocates a `std::function` per press; replace it with a
fixed-capacity pending-action queue only when working in that component. In a
separate deletion pass, remove the caller-less ESP32 APIs, unused window
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

### Shared sample registry and multi-slot residency

SFZ imports still have a private sample registry and only one imported
instrument can be resident independently. Resolve per-slot sample ownership,
unique identities, release behavior, and Sample Manager visibility as the
Sample Pool in the [Track/Instrument model](features/track-and-patch-model.md)
§4 (1024 entries, indexed, fail-with-reason admission — promoted to roadmap
Phase 2.5 item 3), rather than with a local loader patch.

Until that lands, a Track holding an imported Instrument refuses a bare-sample
`Select` (`SfzLoader::BindSample`): the import owns its samples and can only
release them through the load handshake. The UI now states this rather than
appearing to ignore the press, but "replace an Instrument with a sample without
rebooting" needs the refcounted registry and the per-track voice-stop in
[track-and-patch-model.md](features/track-and-patch-model.md) §4.

### Boot-time SFZ autoload (resolved 2026-09-03)

`WAVEX_DAISY_SFZ_BOOT_ENABLED` loaded `WAVEX_DAISY_SFZ_BOOT_PATH` into slot 0
before audio started, so on any card holding that file Track 1 came up owned by
an Instrument on every boot - and therefore refused `Select` for the rest of the
session. That was the mechanism behind the 2026-09-03 bench finding that
"Select does nothing on Track 1".

Now defaulted to 0. Loading an Instrument is a browser action that asks which Track
to use, so nothing claims a Track without being asked. Set the flag to 1 to
restore the old behaviour.

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

### Parameter-id design drift

The target parameter-lock document reserves IDs that conflict with live pan and
pitch values. Reconcile the design with `protocol.h` before implementing
parameter locks or modulation; do not renumber live wire values without a
protocol migration.

## Hardware, diagnostics, and logging

### ESP32 pin verification

The dormant SPI2 assignment collides with PCNT1, and the keypad matrix wiring
is unverified. Resolve both from the schematic and panel/continuity testing
before enabling SPI or treating the keypad as verified.

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
  (image 275 772 to 189 636 bytes). Still owed: the DWT number for
  `Render()` at eight voices and a zero-underrun soak on the `-O2` image;
  `-O3` measured +29 KB over `-O2` and waits for a reason.
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

## Related

- [Roadmap](roadmap.md) — phased work and hardware verification gates.
- [Architecture](architecture.md) — target design and real-time constraints.
- [Inter-MCU protocol](features/inter-mcu-protocol.md) — message ownership and
  reserved ranges.
