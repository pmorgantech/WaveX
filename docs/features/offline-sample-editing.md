# Offline Sample Editing & DSP Mangling — Design

**Status**: Standalone non-destructive persistence is implemented for Phase 1.5.
Destructive rendering below remains target design for Phase 4 in `roadmap.md`.
**Core rule** (from `architecture.md` §1/§6): destructive sample editing and DSP mangling are **offline render jobs**. They never run in the audio callback, and playback must continue glitch-free while a render is in progress.

Sample Edit's non-destructive audition uses `MSG_SAMPLE_AUDITION` (see the
[wire contract](inter-mcu-protocol.md)): the backend resolves a Pool id to its
card file and current metadata, then previews it through the singleton stream.
Audition never changes Track bindings. Samples without a usable card path
cannot use this preview path; a future RAM-only audition needs its own owner.

## 1. Why offline

Time-stretch, pitch-shift, granular processing, and even simple normalize-over-a-3-minute-file cannot be bounded to the 1 ms audio block budget on a 480 MHz M7 that is also mixing 8 voices. Instead of complicating the real-time path with best-effort DSP, we split the world:

- **Real-time (non-destructive, always available)**: playback rate/pitch interpolation, start/end/loop points, per-voice filter/level/envelopes, slice triggering. These are parameters, not data edits.
- **Offline (destructive, rendered)**: anything that produces new sample data. Output goes to a new file (or new sample-RAM object); the source is never modified in place.

## 2. Edit model

### Standalone edits (as-built)

Sample gain follows the same dB law in resident playback and streamed audition:
-24 dB attenuates rather than mutes, 0 dB is unity, and +12 dB amplifies.
The streaming producer uses the vendored CMSIS q15 scale kernel with a Q13
coefficient and saturates PCM overflow; the callback does no gain conversion.

Sample Edit's shifted **Save** opens a confirmation screen and stores trim,
loop, crossfade, gain, fades and the metadata channel mode in `<complete WAV filename>.wxs`
(for example, `Kick.wav.wxs`). **Save As** asks for a new basename and copies
the entire original WAV byte-for-byte plus those edits into the same directory.
Names contain 1–47 ASCII letters, digits, spaces, hyphens or underscores, with
no leading/trailing spaces; the backend appends `.wav`. Existing targets are
never overwritten. Save As leaves the current sample and Track bindings alone;
load the new copy through Browse. It does not crop, normalize or bake gain into PCM.

The sidecar is a WXCF 1.1 type-8 document (1.0 remains readable), defined and tested in
[`sample_file.hpp`](../../firmware/shared/wxcf/sample_file.hpp). It stores file
geometry and edit parameters, excluding runtime IDs, paths, names and Pool
ownership. Bounds and geometry must validate before any edit is applied.
Geometry matching detects length/format changes, not replacement PCM with
identical geometry; this version has no content hash. Missing sidecars mean
unedited defaults. Invalid or unreadable sidecars fail loading instead of
silently playing a different region. A valid `.wxs.bak` can recover a missing
or invalid final sidecar; I/O errors are surfaced.

Fresh standalone WAV loads, newly imported Instrument dependencies and
nonresident stream opens restore the sidecar. An already resident Pool record
keeps its session edits. Project recall uses its captured sample edits and
ignores external sidecars, preserving the Project snapshot's authority.

The Daisy foreground job owns a metadata snapshot and all file handles.
Save As reads/writes at most 4 KiB per pump; callback PCM is unchanged.
The job stops the singleton audition stream, cancels waveform work and excludes
competing storage mutations. Existing resident voices can finish; note-off and
transport Stop remain available. The UI polls correlated status and never
replays a save automatically after lost replies or reconnects. An SD peripheral
I/O failure attempts a lower clock only after the job closes its handles,
keeping 4-bit mode; the failed save remains visible and requires an explicit
retry. Remount can fail after a bus fault; automatic recovery remains open in
[HV-025](../hardware-validation.md#hv-025--standalone-sample-saves).
Boot/insertion starts at the configured fast rate again.

Every output is closed successfully before publication. Save writes a uniquely
named temporary sidecar, renames the prior valid final to `.wxs.bak`, then
publishes the temporary file. The backup is retained until the next save.
Save As publishes its sidecar first and its copied WAV last. Failed operations
remove only files owned by that operation. An interrupted copy can leave an
orphan sidecar or uniquely named temporary file, which blocks name reuse until
inspected/removed on a computer. FAT publication of a WAV/sidecar pair is not a
power-loss-atomic transaction. See [HV-025](../hardware-validation.md#hv-025--standalone-sample-saves)
for the passing two-board save/recall checks and remaining physical recovery
work; host tests do not establish card durability.

### Playback channels

The **CHANNEL** tile selects Recorded, Left, Right or Mono Sum `(L+R)/2`.
Recorded preserves stereo; the other modes select one mono signal. A mono
source remains mono in every mode. Streamed audition sends mono to both
stereo outputs. Resident notes apply the sample selection before the
Instrument oscillator's Mono setting and reserve one render channel for a
mono result, two for stereo. Held notes retain their trigger snapshot;
subsequent notes use the edited selection. The current audition restarts
with the confirmed edit, matching other sample edits.

Channel selection changes neither source PCM nor absolute markers. The
waveform revision advances so cached traces cannot retain the old mapping.
Single traces are labelled Mono, L, R or `(L+R)/2`; stereo retains L/R lanes.
Save/Save As and Project snapshots already persist this metadata field.
Snap and numeric seam checks continue to inspect native source L/R, even
when the selected waveform shows one channel.

### Stereo markers, seam checks and playback crossfade

Touch a parameter tile to select it for the navigation encoder; its push
advances focus. Drag anywhere on a tile, including its fill bar and knob, to
adjust it. Region and loop handles use the continuous waveform while held;
the loop seam view appears after a loop-handle drag is released.

Focus START, END, LOOP START or LOOP END, then use **Shift → Snap**.
The Daisy searches at most 512 source frames either side of the marker.
A stereo candidate must cross zero in **both native channels** at the same
frame boundary; a zero endpoint counts. The mono sum is never used to find a
stereo crossing. Among candidates, choose the lowest worst-channel endpoint
magnitude, then the nearest position, then the earlier position. If no shared
crossing exists, leave the marker unchanged. Trim and minimum loop bounds
constrain the search. The wire API permits a radius up to 2048 frames.

A request carries the content generation and complete expected edit. The
backend rejects a stale edit or missing sample instead of snapping newer
markers. The UI holds further marker edits until that correlated reply or a
two-second timeout; it never automatically replays the mutation. A timeout is
not proof that a snap failed. Check the markers before retrying.

Focus CROSSFADE or either fade control, then use **Shift → Check Seam**.
The two waveform halves display **raw** audio using the selected channel mode at exclusive loop end
and loop start. The numeric check reports the crossfaded source jump separately
for each channel as a percentage of full-scale PCM amplitude, before gain,
region fades and Instrument processing. It does not certify an inaudible splice:
slope, phase, transients and listening still matter.
Crossfade does not rewrite the displayed waveform or its content generation.

CROSSFADE defaults off and accepts 0–20 ms, in one-millisecond steps. The
actual overlap is bounded to 2048 source frames, half the loop length, and a
remaining loop period of at least 256 frames. An overlap shorter than two
frames is off; the info strip shows the effective overlap in frames. Over
those final N frames, linear complementary weights blend the outgoing tail
with the first N loop frames, using exactly the same ramp for both channels.
At exclusive loop end, playback resumes at `loop_start + N`; the repeating
period is therefore `loop_end - loop_start - N`, **shorter than the raw loop**.
The blend has exact endpoints and cannot boost identical correlated signals.
It can attenuate opposing phase; audition before saving. Region fade-out is
suppressed while crossfade is active so it does not fade the blended seam to
silence. Region fade-in is capped to finish by the resumed loop head, so later
passes do not restart that fade and introduce another level discontinuity.

Resident voices hold immutable crossfade parameters per trigger; an edit
changes subsequent notes. Streaming audition uses a fixed 32 KiB foreground
loop-head cache (8 KiB used by Stage A stereo; capacity includes TDM8), populated
through the existing aligned SD staging buffer in at most two reads. Blending
precedes gain, fades and resampling. Editing the
current audition reopens it and discards queued PCM from its old markers;
other auditions and Track bindings are unchanged. Neither path modifies source
PCM or allocates an additional voice. Existing samples and older files default
to crossfade off. Standalone sidecar 1.1 and Project 1.4 retain the setting;
Project recall remains authoritative over external sidecars.

Physical seam listening, display interaction and callback-load evidence are
tracked in [HV-026](../hardware-validation.md#hv-026--stereo-snap-seams-and-crossfade).
These checks do not close the larger Phase 1.5 gate.

### Oversized samples

The Daisy owns resident admission. The frontend does not refuse a load by
comparing container size with cached free RAM: resident path reuse needs no new
PCM allocation, file headers are not PCM, and the allocator can change before
a request arrives. Free-memory telemetry remains diagnostic.

Resident loading is all-or-nothing. Never report a truncated prefix as the
complete sample, evict another Track implicitly, or reinterpret absolute
markers relative to a partial buffer. If the complete allocation cannot be
admitted, retain the current Pool/Track state and report the load failure.
Browser streaming can audition files that do not fit RAM, but Sample Edit
requires a resident Pool identity and the complete sample in RAM. File-backed
editing is out of scope by user decision on 2026-09-21; it is not a Phase 1.5
gate requirement.

### Destructive edits (target)

Slice/choke persistence and the render pipeline below are future work.

**Destructive rendering** happens when the user commits an operation that changes data (normalize, stretch, crush…):

1. UI sends `RENDER_SUBMIT` (new protocol message, Phase 4) with: source path, operation + parameters, destination path.
2. Daisy render scheduler validates, estimates length, replies with a job id.
3. Job runs chunked on the main loop (§3). Progress events → UI progress bar. Cancel supported at chunk granularity.
4. Output is written to `<dest>.tmp`; on success: `f_rename` to final name (after closing the file; power-loss recovery still requires validation), sidecar copied/adjusted, `RENDER_DONE` sent. On failure/cancel: tmp deleted, `RENDER_FAILED` with error code.
5. UI offers A/B audition (source vs render) before replacing pad assignments.

Undo = keep the source file; "save over" is implemented as render-to-temp + rename-swap, and the previous version is retained as `<name>.bak` until the project is saved (bounded to 1 backup per file to cap SD usage).

## 3. Render scheduler (Daisy main loop)

The main loop is already cooperative (`PumpWavIO`, link servicing). Renders join it as a chunked state machine:

```
while (job.active) {                       // one call per main-loop iteration
    read chunk (≤ N frames) from source    // through the same triple-buffer SD slots
    process chunk (CMSIS-DSP kernels)      // budget-bounded, see below
    write chunk to dest .tmp
    update progress counter (report every ~250 ms)
    yield                                   // return to main loop
}
```

**Budget rule**: one render step must complete in ≤ 2 ms wall time (measured with DWT, asserted in debug builds) so link servicing and `PumpWavIO` latency stay bounded. Chunk size is derived from that budget per-operation (a normalize chunk can be much larger than a time-stretch chunk).

**Priority rule**: if streaming playback is active, `PumpWavIO` runs first each loop; render steps are skipped whenever any stream's prebuffer is below its high-water mark. Renders are throughput-elastic; playback is not.

**Memory**: render scratch is the dedicated final 4 MB SDRAM partition reserved in `firmware/daisy/src/sdram_layout.h`, never from the 60 MB sample-RAM arena in use by playback. No heap allocation mid-job.

## 4. Operation catalog (implementation order)

| Tier | Operations | Notes |
|---|---|---|
| 1 — trivial, ship first | trim/crop, gain, normalize, fade in/out, reverse, mono↔stereo, DC removal | single-pass or two-pass (peak scan + apply); validates the whole pipeline |
| 2 — resampling | sample-rate convert, pitch-shift-by-resample (speed change), **crossfade loop** (`xfade_loop(loop_start, loop_end, xfade_ms)` — renders a crossfaded loop seam to a new file + sidecar loop markers; requested by `instrument-model.md` §11 for zone loops) | windowed-sinc polyphase; CMSIS-DSP FIR interpolate/decimate kernels (`arm_fir_interpolate_q15` etc. — one motivation for the CMSIS-DSP 1.17 upgrade, which fixes an OOB coefficient access in FIR interpolation) |
| 3 — character/mangle | bit-crush, drive/saturation, ring-mod against an oscillator, comb/flanger print, filter print | stateless or short-state per chunk; cheap |
| 4 — heavy DSP | time-stretch & pitch-shift (phase vocoder or WSOLA — recommend **WSOLA** first: integer math friendly, no FFT memory pressure), granular freeze/scatter, spectral gate | needs overlap state carried across chunks; design each as a streaming processor with explicit carry buffer |
| 5 — analysis | transient detection for auto-slice, loudness scan, silence trim | feeds the slicer UI; runs as a render job that outputs markers (sidecar), not audio |

The requested [vintage sampler grit](../architecture-notes.md#vintage-sampler-grit)
extends the character tier: 27.7/22.05 kHz rate reduction, companded 8-bit
quantization, explicit pre/post filtering and optional post-filter saturation.
After the basic render pipeline, prepare reduced-rate decoded PCM16 assets for
the requested [virtual-clock playback at fixed 48 kHz](vintage-sampler-math.md).
Live reconstruction and post-filter saturation require their own measured
runtime gate. Complete offline prints remain optional; linear bit-crushing
alone does not satisfy this request.

## 5. Waveform editor UI (ESP32 side)

- **Preview tiers**: Daisy measures min/max envelopes per window (the `MSG_ENVELOPE_REQ`/`MSG_ENVELOPE_CHUNK` path; the `EnvelopeCache` already keeps runs per tier). ESP32 caches tiers in PSRAM keyed by (sample_id, generation); zoom switches tiers, scroll pans within one.
- **Interaction**: touch to place/drag markers, encoder A for fine position (sample-accurate), encoder B for zoom. Softkeys: Trim, Normalize, FX…, Slice, Render.
- **During render**: editor stays interactive on the cached preview; progress bar overlays; audition of untouched regions keeps working.

## 6. What is explicitly out of scope for the real-time path

To keep the boundary crisp, these must **never** be added to the audio callback, however tempting: FFTs of any size, file I/O, sample-format conversion of whole files, allocation, and any operation whose worst case scales with file length rather than block length. If a "live mangling" performance feature is wanted later (e.g. live granular), it gets its own bounded real-time design with preallocated buffers — it does not reuse the offline operations.
