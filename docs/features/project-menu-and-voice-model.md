# Project, menus, mixing and voice channels

**Status:** Stereo/Mono rendering, saved Instrument/Track allocation controls,
Mono held-key fallback and Project persistence are implemented with host,
compile and selected two-board coverage. Listening, physical controls, reboot
recovery and the complete soak gate remain open. Scenes, effects and drift
retain their target/proposal status. Updated 2026-09-20.
This document consolidates the terminology and menu discussion so the remaining
work can be scheduled. It distinguishes current behavior, requested changes and
proposals; the [roadmap](../roadmap.md#next-steps-and-backlog) owns task order.

## Contents

- [Terminology and ownership](#terminology-and-ownership)
- [Menu structure](#menu-structure)
- [Scenes and Songs](#scenes-and-songs)
- [Track and Instrument mixing](#track-and-instrument-mixing)
- [Stereo, Mono and channel capacity](#stereo-mono-and-channel-capacity)
- [Instrument and Kit allocation policy](#instrument-and-kit-allocation-policy)
- [Oscillator drift backlog](#oscillator-drift-backlog)
- [Implementation boundaries](#implementation-boundaries)
- [Related](#related)

## Terminology and ownership

| Entity | Owns or describes | Relationship |
|---|---|---|
| Project | The working session: Track setup, mixer, Patterns, Songs, settings and asset references; Scenes when implemented | Top-level save/load unit |
| Track | Instrument assignment, MIDI routing, mix strip and allocation-policy overrides | 16 per Project; shared selection across pages |
| Instrument | A reusable sound: two oscillators, their maps, filter, amp, envelopes, LFOs and modulation; saved allocation defaults | Loaded into a Track; saved as WXI; a Kit is a drum-mode Instrument |
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

## Instrument and Kit allocation policy

**Implemented 2026-09-20:** Instrument/Kit defaults and independent Track
inheritance/overrides, saved formats, Apply/Revert, confirmed UI controls and
Mono held-key fallback. Physical MIDI latency, audible transitions and the full
capacity gate remain [HV-019](../hardware-validation.md#hv-019--note-group-allocation-policy).
Per-pad caps remain a separate proposal; the global render budget is unchanged.

### Settings and ownership

| Setting | Values | Meaning |
|---|---|---|
| Play mode | Poly / Mono | Mono permits one musical note at a time; independent of the oscillator's stereo-to-Mono downmix switch |
| Polyphony | Auto / 1–8 initially | Maximum simultaneous note groups, including release tails; Auto adds no local cap |
| Steal from | Own only / Own first / Any | Eligible victim scope and preference when capacity is exhausted |
| Kit pad override | Inherit / explicit pad cap and steal policy | Proposed optional per-pad refinement, alongside the overall Kit cap |

The user requested Instrument/Kit controls. Kit-wide plus per-pad limits are
the suggested extension; that finer scope remains a proposal pending user
preference. A one-voice kick and a four-voice cymbal should be possible without
making the whole Kit monophonic. Choke groups still express deliberate
interactions such as a closed hat stopping an open hat.

Sound defaults are saved in WXI's optional Allocation chunk (0x0070, version
1.0; mode/limit/steal and a zero reserved byte). Missing chunks default to
Poly/Auto/Any even if the inert legacy `poly_mode` was nonzero. Projects write
version 1.2 Track chunks: bytes 269–271 hold mode (bit 7 means inherit), limit,
and steal scope. Older Track chunks require zero reserved bytes and default to
Use Instrument. Legacy `poly_limit`/`priority` fields remain inert metadata;
they are never reinterpreted as these settings. Banks embed the same WXI codec.

The Instrument sound undo point includes its policy. Track overrides have an
independent undo point and survive Instrument replacement; a new or loaded
Project resets that undo point. Apply retains edits in the session; Save copy
persists them. A Track override never mutates the Instrument default.

At runtime, "own" means the requesting Track's loaded Instrument instance,
with a binding generation. Two Tracks loading the same WXI remain independent.
For a pad override, own means that pad in that Track instance; **Own first**
can search other pads of the same Kit next, then other Tracks. The aggregate
Kit cap remains an upper bound over all pads. Never key ownership by shared
Sample Pool ID or file path.

Limits are ceilings, not reserved capacity or guaranteed minimum polyphony.
Eight stereo notes may require sixteen render channels; the configured engine
still decides whether those resources exist. A future MCU may expose larger
limits without changing their meaning. Own-only controls what this sound may
steal; it does not prevent other sounds from stealing it. Victim protection,
priority and guaranteed reservations are separate policy questions, not
implicit consequences of a cap.

### Note identity and mono behavior

Use a stable trigger-group identity for every admitted musical note/hit. One
group contains all resolved layer voices and their channel costs. Two
oscillators already submix inside one voice; stereo L/R remain inseparable.
Count the musical group once for the user-facing polyphony limit, while
accounting for every layer slot and render channel against global capacity.
The live sampler and sequencer now submit each resolved note as one group.
Foreground queue admission publishes one compact press with its original Track
mask, regardless of the number of layers. The callback borrows its acquired
immutable prepared map and commits complete layer reservations. Same-frame
sequencer admission uses compact prepared metadata and initializes only surviving
layers, preserving admission order, chokes, identities and random sequences.
Different sample offsets remain separate batches; the measured result and open
capacity checks are in [the callback log](../callback-performance-log.md).

Live presses have FIFO identity per input source and pitch. MIDI channels and
explicitly addressed Tracks are separate sources. A note-off consumes the oldest
unreleased press, including a refused or stolen press, and releases only its
surviving groups. It does not re-route through current MIDI settings or release
sequencer groups. Track replacement revokes queued old-binding triggers without
allowing an old key release to touch a replacement. Velocity-zero Note On is a
release; unmatched offs do not consume future presses. One-shot layers still
ignore normal key releases.

The fixed 64-event queue admits at most 32 routed requests and 32 input events
per callback. With any Mono Track, sixteen routed input requests reserve the
other sixteen admissions for at most one final held-key fallback per Track. Overflow offs retain per-source/pitch serial watermarks, including
older triggers still waiting behind that work limit. Input counters and this
CPU-only handoff live in explicitly initialized DTCM. A source/pitch serial never
wraps into an old identity: after UINT32_MAX presses, new presses on that key
are refused until engine reinitialization. Burst queuing can add multiple blocks
of latency; this bound is not a claim that every burst meets physical MIDI latency.

Mono caps admission at one group. Gated keyboard presses use last-admitted-key
priority and full envelope retrigger. Releasing an older key leaves the current
key alone; releasing the latest retriggers the last admitted key still held.
The callback owns a fixed 64-entry held-key ledger independent of render slots.
Global stealing does not discard held keys. A full ledger refuses new Mono
Track requests without evicting a key or changing note-off identity. Admission
refusals never enter that ledger. Binding retirement and Poly/Mono transitions
clear the affected Track's held keys. All-one-shot groups and drum Instruments
retrigger/replace without fallback. Legato, portamento, continuation and
click-free stealing are not implied by Mono.

Fallback requests coalesce to the final held candidate for each Track at the
end of a callback. A refused fallback is not retried every block; another key
change can cause a new request. Overflow release watermarks cover the held
ledger as well as surviving voices and queued triggers.

### Admission and stealing

Resolve the entire incoming group and its effective policy before mutating
the allocator. Plan a bounded set of whole-group victims, then commit only if
both local caps and the physical slot/channel budgets can be satisfied:

1. Enforce a pad cap, if present, by replacing that pad's own group(s).
2. Enforce the Instrument/Kit cap using groups in that Track instance. These
   cap checks happen **even when there are globally free slots**.
3. Use free global capacity for any remaining resources needed.
4. Under global pressure, apply **Own only** (no external victims), **Own
   first** (local victims before external candidates), or **Any** (global
   candidates using the normal victim ranking).
5. Within an eligible scope, prefer groups entirely in release, then oldest
   onset, with a stable tie-breaker. If none of the permitted victims can free
   enough capacity, reject the new group without partially stealing others.

At a local cap, stealing another Track cannot substitute for removing one of
the capped sound's own groups. Conversely, an own-only sound with no current
notes may be unable to start when other sounds fill the pool. This is expected
policy behavior, not an allocator fault.

Whole-group planning matters when a stereo or layered note needs several mono
victims. Keep selection bounded by configured voice/group capacities; no heap,
locks, filesystem work or partially published policies in the callback. M7
remains the sole runtime allocator if the proposed M4 I/O split is adopted.

Choke release still occupies channels until it ends or is explicitly reclaimed.
Define whether a rejected trigger applies its choke before implementation;
the proposed transactional rule applies choke only for an admitted trigger.
Steal/retrigger transitions need an audible click check and a bounded
transition strategy that includes any temporary tail processing in the global
budget. Do not promise click-free stealing from a metadata-only policy change.

Policy edits apply at a callback boundary. Existing notes can finish; a lowered
cap is enforced on the next attempted admission rather than cutting notes as
the control moves. Legacy files with no allocation settings retain current
Poly / Auto / Any behavior. Explicit old Track fields remain inert metadata as described above.

### Examples and acceptance

| Sound | Example setting | Result |
|---|---|---|
| Bass | Mono, Own only | Replaces its own note; cannot displace another Track to get started |
| Pad Instrument | Poly 4, Own only | Fifth note replaces one of its own; does not steal drums |
| Drum Kit | Poly 8, Own first | Recycles Kit notes before considering another Track under global pressure |
| Kick pad | Pad limit 1, Own only | Repeated kicks replace each other, not a cymbal |
| Cymbal pad | Pad limit 4, Own only | Allows overlapping hits, subject to the aggregate Kit/global budgets |

Instrument editors expose defaults; Project/Track setup exposes inheritance
and overrides; Pad Map exposes adopted pad refinements. Labels must distinguish
**Mono play mode** from **Mono output**. Persist and round-trip policy before
adding UI behavior, and include allocation settings in the appropriate
Apply/Revert snapshot.

Required host cases include local caps with spare global capacity, own-only
refusal without side effects, independent instances of one WXI on two Tracks,
pad/Kit cap interaction, stereo/multilayer whole-group admission, release/choke
accounting, Mono held-key/repeated-note identity, policy changes and old-file
defaults. Hardware gates cover audible stealing/retrigger and worst-case DWT
cost during bursts at full capacity, with sequencer, modulation and audition.
Project's shifted softkeys open **Track poly** and **Sound poly**. Instrument
editor stages other than Oscillator also expose **Polyphony** on Shift. Four
value tiles show inheritance, play mode, group limit and stealing; changing a
Track value creates an override. Confirmed snapshots and revision-checked edits
prevent stale pages from overwriting a replacement. A timeout reads back the
backend state and never automatically retries a mutation.

### Runtime admission foundation — 2026-09-18

[`note_group_admission.hpp`](../../firmware/daisy/src/audio/note_group_admission.hpp)
implements a pure, fixed-capacity victim/reservation planner, used by
`VoiceManager::TriggerGroup`. The callback derives the active snapshot from
live render slots, then commits admission without interleaved mutation. It
uses the resolved saved policy for live and sequencer notes.

| Record | Source of truth and lifetime |
|---|---|
| Owner | Track number plus nonzero binding generation. Shared WAVs and two instances of one WXI do not share ownership. |
| Group | One admitted musical trigger, with a unique increasing 64-bit ID, remaining layer-slot mask, total reserved channels and whether all remaining layers release or will be choked by an admitted request. |
| Request | The entire resolved trigger's slot/channel costs and one already-resolved Poly/Mono, Auto/1–8, Own only/Own first/Any policy. |
| Plan | Whole-group victims, all their retired render slots, and deterministic lowest free slots for the incoming trigger; every failure returns empty masks. |

The callback owns group IDs and Track binding generations. Group IDs also
order onset age; zero is reserved. Exhaustion refuses new notes until engine
initialization with audio stopped; IDs do not reset on Track or transport stops.
Track retirement kills all its voices before advancing its binding generation.
`ReleaseGroup(id)` ignores stale identities and respects one-shot layers.
Live note-offs use source/pitch FIFO identities and cannot release sequencer
groups. As individual layers end, their slots and
channel reservations leave the group; its identity survives until the last layer
ends. Choke/release tails remain charged until actually retired. Victim ranking accounts for the incoming note's prospective chokes without
changing envelopes; a group is eligible as releasing only when all surviving
layers already release or will be choked. Choke effects apply only after
successful admission and before starting any sibling layer,
so a rejected note cannot choke and a layered hit cannot choke itself.

Planning enforces the local cap first, including release tails and even when
global capacity is available. Global pressure then follows the selected scope;
within a scope, entirely releasing groups precede held groups, then older IDs
win. A local cap never evicts another Track as a substitute for an own victim.
The planner validates disjoint slots, unique identities, valid owners and channel
totals before choosing victims. Searches and bit operations are bounded by the
configured capacity (up to 64); there is no allocation, I/O or mutable global
state. That bound is not a measured callback-cost claim.

Host cases cover layered/stereo retirement, cap reduction, foreign release
versus own held victims, binding-generation isolation, rejection after tentative
victim selection, independent channel/slot exhaustion and the highest mask bit.
An exhaustive four-slot test enumerates victim subsets independently to check
feasibility and resource conservation. Cortex-M7 compilation supplements these
checks; audible transitions, callback DWT and the complete workload remain
[HV-019](../hardware-validation.md#hv-019--note-group-allocation-policy).

Remaining work is the audible transition/capacity gate and optional pad policy
refinements. No click-free transition or physical MIDI latency claim follows
from the host and console-injected checks.

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
