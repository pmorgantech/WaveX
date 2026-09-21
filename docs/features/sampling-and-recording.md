# Sampling and recording

**Status:** implemented for codec input and internal master-mix resampling.
Selected two-board workflow checks pass; physical signal, listening, timing,
recovery and full-load acceptance remain open in [HV-030](../hardware-validation.md#hv-030--codec-and-internal-mix-recording).
The user authorized implementation ahead of capacity remediation on 2026-09-21;
the earlier **85.3713%** callback result remains unresolved.

## Contents

- [Signal path and ownership](#signal-path-and-ownership)
- [Save and assignment](#save-and-assignment)
- [Transport and validation](#transport-and-validation)
- [Related](#related)

## Signal path and ownership

`audio/recorder.hpp` is the HAL-free capture core. `recording_session` owns the
foreground/callback handoffs, Sample Pool lifetime and status. `recording_save`
owns cooperative filesystem work. The callback never allocates or accesses SD.

Sources are SAI1 codec stereo, codec left mono, codec right mono and stereo
internal output after the master gain. Optional codec monitoring enters before
master gain. Internal resampling never adds its own monitor feedback path.
CMSIS-DSP converts the selected source to q15. Peak hold, RMS and clipped-frame
count describe that source; output meters retain their separate meaning.

Arm preallocates the take, up to 500 ms of pre-roll, and a 16,384-frame SPSC
ring from SampleMemMgr. Default take length is 30 seconds; allocation admission
may refuse longer takes. Manual Start or a magnitude threshold freezes the
pre-roll prefix, followed exactly once by the trigger frame and live capture.
Maximum length includes pre-roll. The foreground drains the frozen history
before the ring. Overflow stops capture and retains a contiguous prefix; the
producer never advances the consumer index or silently removes audio.

Stop detaches the callback and drains remaining frames before publishing a
normal generation-tagged Sample Pool identity. The unsaved take has no file
path, is pinned, and cannot become a persisted Instrument/Project reference.
Audition uses existing voice admission. Discard waits for audition release
before freeing unsaved PCM. Navigation away from Record does not stop capture.

## Save and assignment

Sample → Record provides source, threshold, pre-roll, maximum length, monitoring,
meters, a take name and Arm/Start/Stop/Audition/Save/Discard controls. Save writes
48 kHz PCM16 to `/wavex/recordings/<name>.wav`, without overwriting an existing
name, plus a `.wxs` sidecar. The writer uses short, exclusive transaction names
and bounded 4 KB data chunks. Deadline WAV reads run before recorder work.
A card operation can still block longer than its normal duration; no fixed
2 ms SD latency guarantee is claimed.

Auto-trim is nondestructive: first/last samples of magnitude at least 33 are
padded by 480 frames, with silent takes retaining their full span. Save binds
the final path to the same Pool identity. Done releases session ownership and
selects the saved sample for the existing Sample Edit/Pool and Instrument
assignment workflows. A dedicated post-recording zone picker is not present.

Write failure leaves the RAM take available for retry or discard. Cleanup
removes only transaction-owned files. Sidecar and WAV are published separately;
the pair is **not power-failure atomic**. Capture is volatile and lost on reboot.
Full-media, interruption and restart recovery remain hardware acceptance work.

## Transport and validation

The typed recorder request/status pair and field definitions are centralized in
[protocol.h](../../firmware/shared/spi_protocol/protocol.h) and mirrored in the
[protocol reference](inter-mcu-protocol.md#recording-arpeggiator-and-performance-controls).
Requests carry take identity; status retains active/completed request identities
and errors. The UI recovers by reading state rather than blindly replaying a
mutation. Busy storage transactions reject incompatible operations.

Host tests cover pre-roll chronology, threshold exactness, overflow, ownership,
WAV bytes, trim math, collisions, protocol dispatch and UI readiness. On
2026-09-21 both codec-stereo and internal-source HIL flows passed capture,
audition, save, Done, Track assignment and unload. These runs do not establish
recorded signal quality, reboot recovery, physical latency or zero-underrun
operation under the full voice/modulation workload. See HV-030 for procedures.

## Related

- [Roadmap](../roadmap.md)
- [Offline sample editing](offline-sample-editing.md)
- [Instrument model](instrument-model.md)
- [Callback measurements](../callback-performance-log.md)
