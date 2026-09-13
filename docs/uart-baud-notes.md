# UART baud and eight-voice timing investigation

**Status:** Limited hardware comparison completed on `experiment/mcu-link-hybrid`,
2026-09-13. The source base is `921151f81c1cd07e7042e4a5ab7476e1de0be258` plus
uncommitted UART experiment changes. Hybrid routing is still a proposal.

## Contents

- [Scope and implementation](#scope-and-implementation)
- [Workloads and interpretation](#workloads-and-interpretation)
- [Measurements](#measurements)
- [Audible timing investigation](#audible-timing-investigation)
- [Image identities and reproduction](#image-identities-and-reproduction)
- [Final hardware state](#final-hardware-state)
- [Evidence and limitations](#evidence-and-limitations)
- [Related](#related)

## Scope and implementation

Compare faster UART before adding a second active transport. Both MCUs derive
their inter-MCU baud from the shared setting in
[`hardware_config.h`](../firmware/shared/config/hardware_config.h), through
existing target aliases in [`pin_config.h`](../firmware/shared/config/pin_config.h).
The default stays unchanged; experimental images must be built and flashed as
a matched pair. Debug/USB console speeds are separate.

Daisy retains 16x oversampling and changes only the UART-local prescaler for
faster presets. The UART kernel, system/audio clocks, DMA assignments, queues,
ISR priorities and audio code are unchanged. Both targets report the actual
configured baud and reject a mismatch greater than 0.5%. Daisy reads the selected
UART clock source and divider; the pinned HAL generic peripheral-clock query
does not implement the UART query and cannot be used for this validation.

Low-rate, profiling-only foreground/task reports expose link health. ESP
reports CRC/sync/sequence, RX buffer/FIFO overflow, framing and parity counters.
Daisy reports CRC/sync/sequence, queue overflow, TX error and resync counters.
Daisy queue overflow includes failed enqueue attempts that producers retry;
that number alone does not establish data loss or an electrical fault. A
debug-only `SAMPLE <id>` console readback copies existing foreground-owned metadata for checking applied regions; it performs no audio work. No
logging was added to the audio callback or interrupt handlers.

## Workloads and interpretation

Keep these scenarios separate when comparing numbers:

1. **Legacy control benchmark:** 100 idle and 100 playing Instrument AMP edits;
   eight looped sample voices, sequencing and SD preview during playback.
2. **Legacy heavy stress:** eight separate drum Instruments, two sources each,
   sixteen pad zones per source, sixty-four modulation routes, sixteen voice
   LFOs, three envelopes, four locks per enabled step, 24 dB full-drive filtering,
   live edits, SD preview and periodic pattern save/load. Backend setup and
   many live edits use the Daisy debug dispatcher. This is a DSP/storage/UI
   workload, not a UART wire-saturation test.
3. **Sustained synth comparison:** eight independent imports of
   `Saw_Synth_Bass.sfz`, sharing its seven resident PCM assets. Each Track has
   one active oscillator, the same note and ADSR, no modulation or locks, a
   fixed Instrument gain of 0.12, and straight eighth-note retriggers at
   120 BPM. No independent preview stream is mixed in. Before measurement,
   note-on/off checks require one voice per Track: 0 through 8 active voices,
   then 8 through 0 as Tracks release individually.
4. **Musical baud comparison:** two bars of one-shot drums on eight Tracks,
   plus four separate keyboard Tracks for a Cmaj7 stab at the start of each bar.
   The eight-voice engine reaches eight simultaneous voices on those downbeats;
   the rest of the groove has variable occupancy. This is not eight continuous
   voices plus four extra chord voices. No independent SD preview is active.

Control timing uses the ESP clock from request admission to matching completed
Instrument state. It excludes UI dispatch and is neither one-way nor audible
latency. Callback load uses DWT windows, weighted by callback count; the peak
is the largest observed callback, not a percentile. The measured audio budget
is 480,000 cycles per 48-frame block at 48 kHz on the 480 MHz Daisy.

## Measurements

The fresh profiling 2 Mbaud pair read back exactly 2,000,000 on both MCUs.
Daisy reported a 120 MHz kernel, prescaler 2 and BRR 30.

| 2 Mbaud reference scenario | Mean | p95 | Maximum |
|---|---:|---:|---:|
| Legacy controls, idle (100 edits) | 748.78 µs | 796 µs | 914 µs |
| Legacy controls, playing (100 edits) | 973.34 µs | 1,278 µs | 2,827 µs |

The fresh legacy heavy run completed 305,086 measured callbacks with zero
reported underruns. Mean callback load was **30.3125%**, maximum **65.5090%**
(145,500 / 314,443 cycles). These are diagnostic results, not the formal
callback-capacity or one-hour stability gate.

The sustained synth load reference completed 305,086 measured callbacks with
zero underruns: **13.5652% mean / 31.2290% maximum** (65,113 / 149,899 cycles).

The user then selected a real drum-and-chord arrangement for the baud comparison.
[`bench_drum_chords.py`](../scripts/bench_drum_chords.py) programs a two-bar
120 BPM groove: eight drum Tracks plus four Cmaj7 note Tracks. All source
loops and independent preview playback are off. Quarter-note chord regions
are 500 ms after accounting for pitch; their ends are sample-region ends,
not a new note-off scheduler. An overlap calculation includes the sequence
wrap and bounds the arrangement at eight voices, reached on accented downbeats.
The melody gate design remains [backlogged](backlog.md#note-lengths-and-one-shot-playback--decision-pending).

### Matched musical controls and load

Each control row contains 100 edits; each load capture lasts about 305 seconds.
Both targets read back the requested rate exactly.

| UART rate | Idle mean / p95 / max | Playing mean / p95 / max |
|---|---:|---:|
| 2 Mbaud | 753.40 / 792 / 991 µs | 858.30 / 941 / 3,020 µs |
| 4 Mbaud | 514.38 / 553 / 696 µs | 601.18 / 711 / 866 µs |
| 5 Mbaud | 469.06 / 513 / 643 µs | 551.81 / 666 / 805 µs |

| UART rate | Callbacks | Mean cycles / load | Maximum cycles / load | Underruns |
|---|---:|---:|---:|---:|
| 2 Mbaud | 305,085 | 29,114 / 6.0654% | 154,703 / 32.2298% | 0 |
| 4 Mbaud | 305,085 | 29,076 / 6.0575% | 162,657 / 33.8869% | 0 |
| 5 Mbaud | 305,086 | 29,237 / 6.0910% | 167,419 / 34.8790% | 0 |

At 4 Mbaud, mean playing control RTT fell by 30.0% and p95 by 24.4%.
Average callback load was effectively unchanged. A single maximum per run does
not establish a change in worst-case load or identify its cause.

Each of the 4 and 5 Mbaud pairs passed four selected two-board HIL cases:
loading and playing a Track, pattern save/load preservation, navigation during
waveform traffic, and sequencer grid/navigation edits. Both completed all 200
control edits and observed eight active voices during the music run, with zero
underruns or debug-log drops.
Reported CRC, sync, sequence-drop, TX-error, resync, ESP framing/parity and ESP
RX-overflow counters stayed zero. Daisy TX enqueue pressure remained visible;
it does not by itself show line corruption or lost messages.

At 5 Mbaud, playing mean RTT was 8.2% lower than at 4 Mbaud and 35.7% lower
than the musical 2 Mbaud reference. The fallback condition did not occur:
3 Mbaud remains compile-verified only. These short trials support a longer
4 Mbaud soak before adding hybrid routing complexity; 5 Mbaud offers a smaller
further control improvement. Waveform throughput was not timed, so this
comparison does not settle the bulk-traffic transport decision.

An initial synth control trial before normalizing the Instrument gains is
retained as `control-uart-baud-2m-synth.json`; it is not the matched comparison.

## Audible timing investigation

The eight-position header strip currently drives only two live **stereo output**
meters. The remaining positions are placeholders, not active-voice indicators;
see [`ui_status_strip.cpp`](../firmware/esp32/components/ui/src/ui_status_strip.cpp).
Two moving bars therefore do not establish that only two voices are rendering.

The user reported a polyrhythm during the drum stress workload. That workload
mixes an independent, free-running SD preview with the eight sequenced Tracks.
Its second kick contains 71,180 bytes of mono PCM16 at 44.1 kHz: about 807 ms
per loop, unrelated to the sequencer’s 250 ms eighth notes. The two oscillator
sources also loop and include detuning/pitch modulation. Extra off-grid attacks
are therefore expected from the test patch; its mixed sound cannot establish
inter-Track trigger skew.

The clean listening comparison removes those confounds and explicitly clears
all Track steps, retrigs, micro-offsets, probability and locks before programming
a straight grid. The hardware hold/release check confirmed all eight Track
owners, with zero reported underruns. Listening feedback and an output capture
are still needed before claiming the reported audible issue is resolved.

The code path schedules step events inside the audio callback, without a UART
round trip for each Track. Equal musical positions produce equal absolute
sample frames, passed to each prepared voice as a within-block offset. A new
host regression in
[`sequencer_voice_map_test.cpp`](../firmware/daisy/tests/unit/audio/sequencer_voice_map_test.cpp)
checks eight Instruments through the scheduler, prepared map and VoiceManager
at 120 and 123 BPM. It checks exact event frames, one surviving voice per Track,
and identical integer/fractional sample positions after every block across
repeated voice stealing. The 123 BPM case exercises nonzero block offsets.
This is software evidence, not a scope/audio capture from the codec.

## Image identities and reproduction

The musical comparison used these profiling images. Each pair has the same
source and arrangement; only the UART rate override differs. These hashes refer
to the application binaries, not the bootloaders.

| Rate | MCU | Application SHA-256 |
|---|---|---|
| 2 Mbaud | daisy | `61858bf8b9fd0c1fb0fba11cd4ebcaab11443b06f005bb94da522b39c1268590` |
| 2 Mbaud | esp32 | `cae24579270013041223e58b0cf03ce8b9c8d84995de6a405f6414fd7707c0f0` |
| 4 Mbaud | daisy | `8e1faba2edc7d551704a7892ec88821065d65e09986e63686d510ff8844ea8e7` |
| 4 Mbaud | esp32 | `fcdbed929adc89ec71221e2c298a414e16d58ac8359bb8f53f345b0297f3a632` |
| 5 Mbaud | daisy | `82a0bf2036d6320cd24ddb7c18e78c66dc8d11e853469e188886180759b6c37c` |
| 5 Mbaud | esp32 | `fcd0c4edffee0cce7129f17ec5de38ab74a18d4e756623d908c34fdea97d7c53` |

Daisy readbacks used a 120 MHz UART kernel: prescaler 2 / BRR 30 at 2 Mbaud,
prescaler 1 / BRR 30 at 4 Mbaud, and prescaler 1 / BRR 24 at 5 Mbaud.
The readback validates programmed clock divisors; it is not an oscilloscope
measurement of the wire.

Use the [baud comparison procedure](flashing.md#uart-baud-comparison) to build
and flash matched images. With both debug consoles and serial loggers running,
run the musical setup from the devcontainer with a Python environment containing
the HIL dependencies:

```sh
python scripts/bench_drum_chords.py --label uart-music
```

The script replaces the current session's Track bindings and pattern, verifies
the applied sample regions and key maps, and leaves the arrangement playing.
Its JSON records the twelve source paths, notes, regions, velocities and hit
positions. Source WAV data is unchanged. The arrangement is a reproducible
session setup; saving only a pattern would not persist the Track Instruments.
Changing tempo would also require recomputing the chord regions.

The local archive includes compiler flags, source SHA-256 values, a source
patch and a copy of the benchmark script. These identify the uncommitted
experiment as well as the base commit.

## Final hardware state

Both boards were restored to the normal, profiling-disabled UART build.
Startup readback confirmed 2,000,000 baud on both. The normal pair passed the
load/bind/keys HIL smoke test, then the musical setup was rerun and left playing
on the Sequencer page at 120 BPM. A final 20-second state check observed eight
voices with zero underruns, zero debug-log drops and no independent preview.

The source default remains unchanged. Faster images and their results remain
available on the experiment branch; no hybrid transport or note-length gate
scheduler was implemented. The normal pair's final readbacks and arrangement
are in `uart-final-normal-2m.json` and `uart-final-normal-2m-arrangement.json`.

| Normal MCU image | Application SHA-256 |
|---|---|
| daisy | `bfaea4ea86340f895ebecee3898da8e78bf64b3a5a2b42ca4152b0f0d0cd21a1` |
| esp32 | `f0603644588125ab4a566eaa1a6c0e588bfadadd538ee5797994a348d60c508f` |

## Evidence and limitations

Local raw artifacts are under `logs/uart-baud-20260913/`; control captures are
under `logs/link-cutover-20260913/`. Both directories are gitignored. Per-trial
JSON records retain image SHA-256 values, compiler flags, source hashes,
configured/readback rates and boot-log offsets. Preserve them outside this
checkout if the local measurement archive is needed later. Per-trial health
reports are snapshots at different times; do not compare cumulative packet or
TX retry totals as if the runs had identical durations and polling histories.

The first attempted profiling 2 Mbaud setup failed because the generic HAL
clock query returned zero. It is recorded as `invalid_setup`, not as an
unclean baud rate. After correcting the query, both boards linked normally.
A subsequent host Python environment lacked pytest; the control test was
rerun with the existing HIL virtualenv on the same images.

Required pre-commit checks passed: both normal firmware builds, all three host
test suites, formatting and Python lint. The selected HIL cases ran on the
actual two boards at each candidate rate; they are not the full Phase 2 gate.

No completed baud trial by itself proves worst-case MIDI/audio latency,
electrical margin over temperature or other wiring, a one-hour SD soak, or
waveform goodput. Distinguish UART RX line errors from producer queue pressure
and storage failures. A faster wire cannot remove application queueing or DSP
cost. Keep the known matched rollback images until the candidate passes its
hardware checks.

## Related

- [UART/SPI findings](spi-notes.md)
- [Proposed hybrid design](features/hybrid-inter-mcu-link.md)
- [Build and cutover procedure](flashing.md#uartspi-comparison)
- [Performance measurement](performance_monitoring.md)
