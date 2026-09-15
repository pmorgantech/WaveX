# Project, menus, mixing and voice channels

**Status:** Stereo/Mono allocation and Project Track controls implemented;
host/compile verified, hardware verification open. Scene, session persistence,
effects and drift sections retain their target/proposal status. Updated 2026-09-14.
This document consolidates the terminology and menu discussion so the remaining
work can be scheduled. It distinguishes current behavior, requested changes and
proposals; the [roadmap](../roadmap.md#next-steps-and-backlog) owns task order.

## Contents

- [Terminology and ownership](#terminology-and-ownership)
- [Menu structure](#menu-structure)
- [Scenes and Songs](#scenes-and-songs)
- [Track and Instrument mixing](#track-and-instrument-mixing)
- [Stereo, Mono and channel capacity](#stereo-mono-and-channel-capacity)
- [Oscillator drift backlog](#oscillator-drift-backlog)
- [Implementation boundaries](#implementation-boundaries)
- [Related](#related)

## Terminology and ownership

| Entity | Owns or describes | Relationship |
|---|---|---|
| Project | The working session: Track setup, mixer, Patterns, Songs, settings and asset references; Scenes when implemented | Top-level save/load unit |
| Track | Instrument assignment, MIDI routing, mix strip and future polyphony policy | 16 per Project; shared selection across pages |
| Instrument | A reusable sound: two oscillators, their maps, filter, amp, envelopes, LFOs and modulation | Loaded into a Track; saved as WXI; a Kit is a drum-mode Instrument |
| Oscillator | One sound source and its source settings | Two per Instrument; sampled stereo can use one oscillator |
| Sample / Zone | Sample is PCM plus metadata; Zone maps a Sample to key/velocity ranges | Zones reference Sample Pool entries; files persist asset references |
| Pattern | Musical events and parameter locks addressing Tracks | Project codec supports 128 named slots; Pattern changes preserve Track assignments |
| Song | An ordered arrangement of Pattern references and repeats | Project codec supports 16 Songs, each with up to 128 entries |
| Scene | A named recallable snapshot of selected Project settings | Project-owned; proposed initial capacity: eight slots |
| Bank | A numbered collection of saved Instruments | Storage and recall; loading makes a Track-owned editable copy |
| Voice | One resolved note/layer instance with sample cursors, envelopes and modulation | Temporary engine allocation; a stereo voice uses two render channels |
| Render channel | One mono lane of a voice's audio processing | Allocated from a configurable global budget; distinct from Tracks, MIDI channels and hardware outputs |

**Performance needs no separate saved object.** Its current meaning is the live
Track and mixer setup already owned by Project. Existing design documents use
“one Performance per Project”; read that as Project settings, not another file
or a second independently managed hierarchy. The existing Project data type
already stores Tracks directly.

Project persistence currently has a host-tested data model and codec. Device
save/load, complete session restore and Song execution remain unfinished.
There are no implemented Scene slots or Scene recall controls yet.

## Menu structure

The former Track page already provided central Instrument assignment and MIDI
routing. Renaming it Performance and adding Track level/pan was the first step.
The root is now **Project**, exposing assignment, MIDI, level, pan/balance and
mute. Project is the setup owner; there is no separate Performance save object. It is useful independently of sequencing and should remain globally
accessible.

Navigation direction (Tracks/Mixer is exposed directly on Project today):

```text
Project
  Tracks / Mixer   — assign Instruments, MIDI routing, levels, pan, mute/solo
  Scenes           — store, name and recall Project snapshots (future)
  Save / Load      — manage the complete session (future device integration)
Instrument         — edit/save the selected Track's sound; browse Instruments/Banks
Sequencer
  Pattern          — events, steps, locks and Pattern management
  Song             — arrangement and section recall (future)
```

This shows the affected areas, not a replacement inventory of every existing
menu. Project now contains the existing setup page; separate tabs for future
Scene and session-storage workflows remain a UI decision. No separate Performance save or
Scene-as-Project container is needed. Track event editing belongs in Sequencer;
Track assignment and mixing belong in Project. Preserve the selected Track
when moving between these views.

## Scenes and Songs

Proposed first version: **eight named Scenes per Project**, each capturing the
16 Tracks' level, pan and mute settings. Eight is an initial product proposal,
not an implemented storage limit. Future additions can include macro values,
effect sends and selected modulation parameters after those systems exist.

Use explicit **Store/Update Scene** to capture current settings. Editing a
fader after recall changes the live setup; it does not silently overwrite the
stored Scene. Show the active Scene, modifications and any queued recall.

A Scene does not reload Instruments, sample maps or PCM. Those belong to the
Project's prepared working set. Recall must use resident state and publish one
coherent change at an audio/control boundary, with gain ramps to avoid clicks.

Proposed recall behavior:

- Manual recall defaults to the next bar during playback; while stopped, apply
  at the next control boundary. Immediate recall can be a later explicit option.
- A Song entry may reference a Scene alongside its Pattern. For example,
  “Verse: Pattern 3, four repeats, Scene 1” then “Chorus: Pattern 7, Scene 2”.
- Apply the Scene once on entering that Song entry. Repeats retain subsequent
  live edits; an entry without a Scene leaves the current mix alone.
- Keep Pattern selection separate from Scene state initially. The older Scene
  draft's optional Pattern reference is an alternative, not a second authority
  over Song transitions.

Before implementation, settle stop/seek behavior, queued-recall cancellation,
automation precedence, missing/deleted Scene references and whether restarting
an entry reapplies its Scene. Save/reload must preserve stable slot identities.

## Track and Instrument mixing

The Instrument controls its sound character and preset loudness. The Track
places that sound in the Project's mix. They compose rather than overwrite one
another: Instrument gain 0.5 with Track gain 0.5 gives gain 0.25 before other
velocity, envelope and master factors. Two Tracks can use the same Instrument
preset with different mix levels without changing the saved preset.

Project's mute control edits a user mute target. Sequencer Solo has a separate
temporary mask: manual mutes take priority, and clearing Solo restores current
mute targets, including edits made while soloed. Pattern row mute separately
stops row triggering and does not replace the Project mix.

Current pan combines Instrument/zone placement with Track pan offset and
clamps the result. For the stereo implementation, interpret that effective
control as balance; for mono, retain the current linear pan law.

Track EQ and effects are future work. Instrument filtering remains part of
sound design, per voice. Future Track EQ/inserts process the Track sum; Track
send amounts route that mix to shared effect buses, whose returns feed the
master. Exact EQ, insert/send order and pre/post-fader options require a DSP
budget and an explicit design. A level/pan implementation does not imply those
processors already exist.

## Stereo, Mono and channel capacity

**Implemented:** Stereo samples preserve both channels by default. Each
oscillator exposes a **Mono** boolean, saved with the Instrument, default Off. On explicitly
combines stereo to `(L + R) / 2`; a native mono sample remains mono in either
setting. PCM on disk is unchanged. This is independent of mono/legato keyboard
polyphony modes.

One oscillator can read a stereo waveform: its left and right samples share
one playback cursor, rate, loop and envelope timing. Stereo requires independent
left/right filter state. Two oscillator sources feed the note's submix; they do
not automatically mean two voices.

Reserve channels for the lifetime of the note, including release:

| Resolved sources in a note | Channel cost |
|---|---:|
| Native mono and/or stereo sources with Mono enabled | 1 |
| Either oscillator preserves a stereo source | 2 |

A zero-level oscillator can become audible through live edits, so level zero
alone must not release its reservation. A layered Instrument can resolve one
note into several voices; each voice consumes its own channel allocation.

The global render budget is `WAVEX_AUDIO_CHANNEL_BUDGET` in
[hardware_config.h](../../firmware/shared/config/hardware_config.h), with a
default of eight. Allocation and fixed storage derive from configuration,
so a future MCU can support more without finding hard-coded limits. `WAVEX_NUM_VOICES` counts note slots and defaults to the channel budget,
so increasing the budget also increases all-mono capacity. A separate note-slot
override may impose a lower capacity.

Examples at the requested default: eight mono voices, four stereo voices, or
three stereo plus two mono voices. Admission must never exceed the budget.
Retain deterministic stealing, preferring release tails then oldest voices;
a stereo trigger may need to steal two mono voices. Mono changes apply to new
notes, keeping held notes' channel reservations stable.

Stereo balance at center preserves both channels. Turning left attenuates
right and vice versa; hard left silences the right channel rather than moving
its content left. Forced mono uses the existing downmix and mono pan behavior.

RAM Instrument voices now preserve stereo through their submix, independent
filters and shared envelope; forced Mono retains the previous downmix behavior.
A mono source is centered at half gain on each side when paired with a stereo
source, preserving its center level. Streaming audition remains its separate
stereo path and does not inherit an Instrument oscillator's Mono setting.
Sample metadata `channel_mode` controls and RAM-path reconciliation remain
separate Sample Edit work. The channel
budget describes Instrument rendering, not the number of codec outputs or a
promise that audition, FX and other callback work have no cost.

## Oscillator drift backlog

**Backlog only, requested 2026-09-14.** Emulate small pitch differences between
voices and between an Instrument's oscillators. Default amount should be zero,
so existing sounds retain their tuning.

Candidate behaviors:

- **Note-on variation:** sample an independent signed random pitch offset for
  each voice/oscillator at note-on; hold it for that note's lifetime.
- **Continuous drift:** evolve a slow, smooth independent signal per
  voice/oscillator, using a low-frequency oscillator or smoothed random targets.
- Allow either behavior or a controlled blend, bounded by an amount in cents.

A single Instrument-level amount could scale independent variation for both
oscillators; optional per-oscillator amounts would offer finer control. A
global device control could be a master scale, but must not silently become
another owner of saved Instrument tuning. Scope, rate, distribution, retrigger,
seed/reproducibility and whether combined modes share one total pitch limit
remain decisions. Stereo L/R must share the same drift to preserve their phase
relationship. Reuse the existing oscillator pitch modulation path where useful;
keep random generation and smoothing bounded and measure callback cost before
promoting this work into the roadmap phase.

## Implementation boundaries

Schedule from the [canonical roadmap](../roadmap.md#composition-and-performance-workflows):
complete hardware verification of channel allocation and Mono renderer/UI
changes, then continue Project/Mixer integration within the
current Phase 2 dependencies. Scenes/macros and effects retain Phase 5 placement.
Drift remains unscheduled.

Stereo acceptance includes independent L/R interpolation/filtering, explicit
Mono downmix, mixed-cost stealing and release accounting, alternate macro
capacity builds, Instrument save/load and Apply/Revert, and audible pan/balance.
Measure before/after DWT callback load at the same workload, then test the full
budget with both oscillators, streaming audition and active modulation. Host
checks and compile success do not close the hardware or one-hour soak gates.

## Related

- [Track and Instrument model](track-and-patch-model.md)
- [Project persistence](project-persistence.md)
- [Output routing and mixer](output-routing-and-mixer.md)
- [Scene and macro draft](scenes-and-performance.md)
- [Oscillator source types](oscillator-sources.md)
- [Callback performance evidence](../callback-performance-log.md)
