# Offline Sample Editing & DSP Mangling — Design

**Status**: Target design (nothing implemented yet). Phase 4 in `roadmap.md`.
**Core rule** (from `architecture.md` §1/§6): destructive sample editing and DSP mangling are **offline render jobs**. They never run in the audio callback, and playback must continue glitch-free while a render is in progress.

## 1. Why offline

Time-stretch, pitch-shift, granular processing, and even simple normalize-over-a-3-minute-file cannot be bounded to the 1 ms audio block budget on a 480 MHz M7 that is also mixing 8 voices. Instead of complicating the real-time path with best-effort DSP, we split the world:

- **Real-time (non-destructive, always available)**: playback rate/pitch interpolation, start/end/loop points, per-voice filter/level/envelopes, slice triggering. These are parameters, not data edits.
- **Offline (destructive, rendered)**: anything that produces new sample data. Output goes to a new file (or new sample-RAM object); the source is never modified in place.

## 2. Edit model

**Non-destructive first**: markers (start, end, loop, slice points), gain, and choke settings live in a **sidecar file** (`<sample>.wxs`, versioned struct or msgpack) next to the WAV on the Daisy SD card. The engine applies them at trigger time; the UI edits them live with no rendering.

**Destructive rendering** happens when the user commits an operation that changes data (normalize, stretch, crush…):

1. UI sends `RENDER_SUBMIT` (new protocol message, Phase 4) with: source path, operation + parameters, destination path.
2. Daisy render scheduler validates, estimates length, replies with a job id.
3. Job runs chunked on the main loop (§3). Progress events → UI progress bar. Cancel supported at chunk granularity.
4. Output is written to `<dest>.tmp`; on success: `f_rename` to final name (atomic on FatFs at the directory-entry level), sidecar copied/adjusted, `RENDER_DONE` sent. On failure/cancel: tmp deleted, `RENDER_FAILED` with error code.
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

**Memory**: render scratch comes from a dedicated SDRAM extent reserved at boot (e.g. 4 MB), never from the sample-RAM pools in use by playback. No heap allocation mid-job.

## 4. Operation catalog (implementation order)

| Tier | Operations | Notes |
|---|---|---|
| 1 — trivial, ship first | trim/crop, gain, normalize, fade in/out, reverse, mono↔stereo, DC removal | single-pass or two-pass (peak scan + apply); validates the whole pipeline |
| 2 — resampling | sample-rate convert, pitch-shift-by-resample (speed change) | windowed-sinc polyphase; CMSIS-DSP FIR interpolate/decimate kernels (`arm_fir_interpolate_q15` etc. — one motivation for the CMSIS-DSP 1.17 upgrade, which fixes an OOB coefficient access in FIR interpolation) |
| 3 — character/mangle | bit-crush, drive/saturation, ring-mod against an oscillator, comb/flanger print, filter print | stateless or short-state per chunk; cheap |
| 4 — heavy DSP | time-stretch & pitch-shift (phase vocoder or WSOLA — recommend **WSOLA** first: integer math friendly, no FFT memory pressure), granular freeze/scatter, spectral gate | needs overlap state carried across chunks; design each as a streaming processor with explicit carry buffer |
| 5 — analysis | transient detection for auto-slice, loudness scan, silence trim | feeds the slicer UI; runs as a render job that outputs markers (sidecar), not audio |

## 5. Waveform editor UI (ESP32 side)

- **Preview tiers**: Daisy generates decimated min/max peak pyramids per file (existing `MSG_PREVIEW_REQ`/`MSG_WAVE_CHUNK` path, extended with a tier parameter). ESP32 caches tiers in PSRAM keyed by path+mtime; zoom switches tiers, scroll pans within one.
- **Interaction**: touch to place/drag markers, encoder A for fine position (sample-accurate), encoder B for zoom. Softkeys: Trim, Normalize, FX…, Slice, Render.
- **During render**: editor stays interactive on the cached preview; progress bar overlays; audition of untouched regions keeps working.

## 6. What is explicitly out of scope for the real-time path

To keep the boundary crisp, these must **never** be added to the audio callback, however tempting: FFTs of any size, file I/O, sample-format conversion of whole files, allocation, and any operation whose worst case scales with file length rather than block length. If a "live mangling" performance feature is wanted later (e.g. live granular), it gets its own bounded real-time design with preallocated buffers — it does not reuse the offline operations.
