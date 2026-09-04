# Oscillator Source Architecture

This document defines how WaveX distinguishes sampler and wavetable oscillator
sources even though both ultimately read stored pulse-code modulation (PCM)
data. Use it when extending Instrument persistence, note resolution, voice
rendering, asset management, or modulation.

**Status:** Architectural boundary accepted; sampler source partially built,
wavetable source deferred and unscheduled. This document does not promote
wavetable work into the current roadmap phase.

## Contents

- [Decision](#decision)
- [Vocabulary](#vocabulary)
- [Common voice path](#common-voice-path)
- [Sampler source](#sampler-source)
- [Wavetable source](#wavetable-source)
- [Asset and persistence rules](#asset-and-persistence-rules)
- [Real-time rules](#real-time-rules)
- [Roadmap placement](#roadmap-placement)
- [Open design decisions](#open-design-decisions)
- [Related](#related)

## Decision

PCM is an asset encoding, not an oscillator type. WaveX models sampler and
wavetable playback as distinct, typed oscillator sources inside an Instrument.

A sampler interprets PCM as an arbitrary-length recording with a region and
optional loop. A wavetable interprets PCM as a validated matrix of fixed-length
single-cycle frames, with a wrapping phase and a continuously modulatable table
position. Sharing file readers, memory allocators, interpolation utilities, or
registry infrastructure must not collapse those two contracts.

This distinction preserves a stable Instrument and Track hierarchy while allowing a
future hybrid Instrument to use different source types. It also prevents
sampler-only concepts such as slice maps and loop markers from leaking into the
wavetable renderer, or wavetable-only concepts such as frame position from
leaking into sample Zones.

## Vocabulary

| Term | Meaning |
|---|---|
| **Asset** | A file or generated data on storage. WAV is one possible container for both recordings and tables; the importing context and validated metadata establish its type. |
| **Oscillator** | One of an Instrument's two typed slots (`Instrument::osc[2]`, `track-and-patch-model.md` §3.1): a recipe for turning an asset into a signal. It holds source-specific configuration but no active playback cursor. `OscType` reserves 8 values; `Off`, `Sample` and `Wavetable` are defined. |
| **Instrument** | The saved playable sound (`.wxi`): two oscillators, submix, filter, amp, three envelopes, two per-voice LFOs, mod matrix, and later effects. |
| **Voice** | A runtime render allocation created by a note or trigger. It carries one active cursor per oscillator plus modulation state and is never a saved user object. |
| **Zone** | A Sample-oscillator mapping from key and velocity ranges to one sample asset. It is not the generic base class for every source type. |
| **Kit** | A drum-mode Instrument whose Sample-oscillator Zones form a pad map. It is not a separate layer above Instrument. |

## Common voice path

All note-producing inputs converge on the same ownership path:

```text
MIDI / pad / sequencer
        |
        v
      Track ---------------> Track mixer and routing
        |
        v
      Instrument
        |
        v
typed source resolution -> Voice allocation -> common post-source processing
                                              (gain, filter, envelopes, routing)
```

The Track determines where a note is addressed and which Instrument is active. The
Instrument resolves the note into one or more source triggers. `VoiceManager` owns
allocation and stealing. The chosen renderer owns only the per-voice source
state needed to produce PCM for the common downstream path.

The source boundary may support more source types later, but it must remain a
small, fixed-capacity, allocation-free dispatch in the audio path. It must not
use RTTI, exceptions, heap-backed polymorphism, or storage I/O.

## Sampler source

A sampler source accepts arbitrary-length audio: transients, speech, drum hits,
long loops, and multisampled instruments. Its Instrument representation is the
existing `Instrument`/`Zone` model.

Sampler behavior includes:

- start and end frames;
- one-shot, gated, forward-loop, and eventually ping-pong modes;
- optional loop-join fade or playback crossfade;
- pitch from playback-rate change, with offline time-stretch available as a
  separate render operation;
- key ranges, velocity layers and crossfades, future round-robin policy, and
  choke groups;
- RAM residency for short samples and bounded streaming for long samples.

The current code implements the RAM-resident core and a separate singleton
streaming path. Concurrent streamed voices and the unified registry remain
open roadmap work.

## Wavetable source

A wavetable source accepts a validated table of fixed-length single-cycle
frames. A phase accumulator addresses samples within the active frame and wraps
at the cycle boundary. Wavetable position selects adjacent frames and is a
continuous modulation destination; the renderer interpolates both within a
frame and between frames according to the eventual quality policy.

Wavetable behavior includes:

- fixed cycle length and frame count recorded as imported metadata;
- pitch from phase increment, independent of the table-position scan;
- a normalized table-position parameter (`WT_POS1`/`WT_POS2`, one per
  oscillator slot) as a mod-matrix destination for envelopes, LFOs, parameter
  locks, and macros — the accumulator the user "scrubs";
- complete RAM residency before an Instrument becomes playable;
- import-time validation and preprocessing rather than repair or file parsing
  during note-on.

A short looping sample is not automatically a wavetable. Converting a recording
into a table is an explicit offline import/render operation that slices or
resynthesizes the source into valid cycles. Likewise, a timed list of samples
with per-step duration, pitch, rest, or crossfade is a future wave-sequence
source, not a wavetable scan.

## Asset and persistence rules

- The SD library may store samples and wavetables together, but every loaded
  asset has an explicit validated type and immutable metadata snapshot.
- Saved Instruments reference assets by stable path; runtime ids never appear in
  `.wxi` files.
- The existing sample registry remains the sampler residency authority until a
  broader typed asset registry is deliberately designed. Do not bolt wavetable
  entries onto it without defining identity, lifetime, reference counts, and
  Sample Manager visibility.
- Unknown future WXCF chunks remain skippable. A future wavetable source chunk
  must be versioned and must not overload the sampler `ZONE` representation.
- Project snapshot/export may copy referenced assets for portability, but
  ordinary saves keep references instead of duplicating PCM into every Pattern
  or Song.

## Real-time rules

Both source types obey the existing callback rules:

- allocate memory, open files, validate formats, and preprocess data outside
  the audio callback;
- publish a complete immutable source descriptor before making it selectable;
- keep per-voice state fixed-size and owned by one Voice;
- perform only bounded reads in the callback; sampler streaming consumes a
  prepared ring, while wavetables read resident tables;
- preserve sequential or cache-friendly SDRAM access and measure the renderer
  with the DWT cycle counter under the full voice load;
- reject an asset cleanly if its memory or CPU admission check fails.

## Roadmap placement

Sampler playback, Instrument editing, and the Track/Instrument workflow remain the
current product path. The wavetable renderer is an unscheduled, post-Phase-2.5
candidate and must be promoted from `backlog.md` before implementation. This
follows the version-1 rule: establish reliable sampling, sequencing, Instrument
persistence, and zero-underrun performance before adding another oscillator
engine.

The common typed-source seam may be extracted earlier only when a scheduled
sampler change already touches the same boundary and the extraction is small,
host-tested, and behavior-preserving.

## Open design decisions

Before wavetable implementation, its focused design must settle:

1. supported import containers and how cycle length/frame count are declared;
2. canonical internal frame length and maximum frame count;
3. interpolation and anti-aliasing or mipmap policy;
4. whether an Instrument initially supports one wavetable source or multiple layered
   sources;
5. modulation ranges, phase-reset behavior, unison, warp, and frequency
   modulation scope;
6. RAM admission limits and DWT budget at the supported polyphony;
7. WXCF source chunks and backward-compatible Instrument loading;
8. browser, editor, and waveform/table visualization requirements.

## Related

- [System architecture](../architecture.md)
- [Track and Instrument model](track-and-patch-model.md)
- [Instrument model](instrument-model.md)
- [Parameter locks and modulation](param-locks-and-modulation.md)
- [Offline sample editing](offline-sample-editing.md)
- [Roadmap](../roadmap.md)
- [Backlog](../backlog.md)
