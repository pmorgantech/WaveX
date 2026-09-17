# Sampler and synth architecture notes

These notes collect reference-instrument ideas worth considering for WaveX.
They are exploratory comparisons, not accepted designs or implementation tasks;
the [roadmap backlog](roadmap.md#sampler-and-synth-architecture-references) owns
promotion and scheduling. Use them when revisiting navigation, Instrument
expression and performance recall.

## Contents

- [ASR-10 reference and assessment](#asr-10-reference-and-assessment)
- [Navigation and edit context](#navigation-and-edit-context)
- [Ownership and terminology](#ownership-and-terminology)
- [Performance variations and expressive voices](#performance-variations-and-expressive-voices)
- [Playback modulation and offline commands](#playback-modulation-and-offline-commands)
- [E4 / EOS reference and assessment](#e4--eos-reference-and-assessment)
- [EOS Cords: further modulation lessons](#eos-cords-further-modulation-lessons)
- [Comparing the useful ideas](#comparing-the-useful-ideas)
- [Workflow coverage and remaining design gaps](#workflow-coverage-and-remaining-design-gaps)
- [Vintage sampler grit](#vintage-sampler-grit)
- [Fit with WaveX's roadmap](#fit-with-wavexs-roadmap)
- [Related](#related)

## ASR-10 reference and assessment

Reference: the user-supplied ASR-10 menu, data-model and voice-path summary,
2026-09-17. Hardware limits, exact menu codes and historical DSP details have
not been independently checked against the manual; this document preserves
design lessons rather than an ASR compatibility specification.

The most useful idea is a predictable relationship between **the selected
musical object, a parameter page and an action**. The supplied design also
connects sample mapping, synthesis and live variation particularly well.
WaveX can borrow those interaction ideas while retaining its own ownership,
stereo handling and bounded audio work (principles 1, 5, 6, 12 and 14).

## Navigation and edit context

The ASR's Mode + Page + parameter address makes expert navigation repeatable.
For WaveX, favor stable root jumps and visible tabs/softkeys, retaining the
selected Track and sound context across them. A proposed context such as
`T3 · Piano / Osc 2 / Zone 7` would make the target and scope of an edit clear.
It describes UI selection, never an allocated runtime Voice.

Keep three kinds of action distinguishable within the existing navigation:

| User intent | WaveX interpretation |
|---|---|
| Browse / load | Preview an asset, then explicitly assign it to a Track or Zone |
| Edit | Adjust the selected sound through the existing preview and Apply/Revert model |
| Command | Explicitly copy, clear, save or render the selected object, with scope and replacement behavior visible |

This does not require three global modes. The current touchscreen offers room
to show context and actions together. Avoid duplicating every page in a mode
matrix or relying on a double-click or flashing label to distinguish files
from resident sounds. Direct shortcuts should supplement visible navigation.

**Candidate: select-by-playing.** An explicit selection mode could locate a
Zone from a key/pad and velocity, then pin it for editing. Ordinary playing
must not silently move the edit target. Overlapping Zones and two oscillators
require a visible choice among matches; display the stable Zone slot, not a
transient voice number. This remains a proposal.

## Ownership and terminology

ASR names cannot be transferred literally. Use WaveX's existing entities:

| ASR concept in the supplied summary | Closest WaveX concept and boundary |
|---|---|
| Bank restores the machine's setup and sequencer | **Project** owns session persistence; WaveX **Bank** stores numbered Instruments |
| Instrument selected by a Track button | **Track** owns an editable Instrument binding and independent mixer/MIDI state |
| WaveSample | Responsibilities split between **Sample asset**, **Zone** mapping/playback settings, Instrument synthesis and temporary **Voice** state |
| Layer | No direct container equivalent; oscillator Zone maps already express key/velocity selection and bounded layering |
| Patch Select layer mask | Possible Instrument-local variation; not a new name for an Instrument or Project Scene |
| Performance Preset | Related to Project/Scene recall, but ASR contents exceed the proposed initial level/pan/mute Scene scope |

Do not insert an eight-Layer hierarchy without a musical requirement the
existing two-oscillator model cannot express. A group container would need
explicit identity, membership, parameter inheritance and note-lifetime rules
before algorithms or storage changes.

Stereo is already one coherent source with shared playback timing and separate
channel processing. ASR companion-layer editing is a useful historical lesson
in keeping edits coherent, not a reason to split WaveX stereo assets into
separately editable layers. Layered notes still consume the existing measured
render-channel budget.

## Performance variations and expressive voices

**Quick sound variations are worth revisiting.** Patch Select demonstrates
the value of changing a resident sound without navigating or loading files.
For example, “body only” and “body + attack” could use existing oscillator
controls. First determine whether macros can express the desired gesture;
only introduce stored Instrument variations if they add something distinct.
Project mix changes remain Scenes. Neither operation should silently rewrite
the saved Instrument or Scene.

Before adopting variations, decide what is captured, whether recall affects
held notes or only new notes, interaction with locks/modulation, and how the
active/edited variation is shown. Any engine handoff must be coherent and
bounded, with resident sample references held for sounding notes.

**Velocity can shape articulation, not just loudness or sample choice.** The
ASR's soft/hard envelope contours suggest useful musical examples: a harder
strike could shorten attack while changing the filter contour. First assess
what existing velocity routes and Zone selection can express. Dual-contour
or multistage envelopes need a separate proposal defining interpolation,
time units, retrigger/release behavior, persistence and callback cost.
Envelope templates may offer a smaller usability improvement using the current
ADSR model, with an explicit target and undo behavior.

Keep WaveX's envelope identity: **Env 1 owns amplitude**, Env 2/3 provide
additional modulation, and all three are matrix sources. Do not copy ASR
numbering or its amplitude-envelope routing restriction. Likewise, key-up
and legato-triggered articulations need explicit note ownership and bounded
allocation before becoming Zone/group features; they remain unscheduled.

## Playback modulation and offline commands

The ASR Wave/Command distinction reinforces WaveX's separation between live
playback parameters and offline PCM processing. Keep trim/loop markers distinct
from rendering crossfades, normalization, time-stretch or destructive edits.
Render jobs publish completed assets through the existing ownership boundary;
they never edit PCM underneath held voices.

Transwave is useful inspiration for expressive source-position control, but
it must not collapse Sample loops and Wavetable frames into one representation.
Keep the accepted [typed source boundary](features/oscillator-sources.md).
Future loop-position modulation needs bounds, seam/click behavior and streaming
residency rules; wavetable scanning needs validated resident frames. Neither
follows automatically from exposing a modulation destination.

The two-filter ASR path is another possible sound-design reference, not a
reason to expand WaveX's filter topology or couple effects selection to sample
rate/polyphony. Any such DSP change needs a musical case and measured capacity.

## E4 / EOS reference and assessment

Reference: the user-supplied mature E4/EOS architecture summary, 2026-09-17.
As with the ASR reference, version-specific limits, Cord counts, import support
and exact DSP behavior are not comprehensively verified here; the focused
[Cord follow-up](#eos-cords-further-modulation-lessons) cites selected manual checks. The useful
comparison is the separation of **sample assets, mapping, shared synthesis
settings and runtime note instances**.

### Multisample maps share a sound definition

EOS's multisample Voice is a useful editing concept: one set of synth settings
can shape a whole keyboard map. In WaveX, Instrument-owned synthesis and the
oscillators' Zone maps already provide much of this separation. A proposed
workflow such as `Instrument → Key Map → Sound` should preserve the Track and
oscillator selection and clearly label whether a control changes the whole
Instrument or a Zone override.

“One synth engine over the map” describes a shared sound definition. It must
not be interpreted as sharing mutable filter/envelope state across sounding
notes. WaveX's runtime Voice remains a temporary renderer for a resolved
note/layer, with its own state and stereo channel cost. An EOS stored Voice
definition is therefore not the same entity as a WaveX runtime Voice.

**Group editing is a strong candidate.** Selecting several Zones could support
“tune these samples together” without introducing a new synthesis layer. An
edit selection is a set of stable oscillator/Zone identities tied to an
Instrument revision. Show mixed values and distinguish setting an absolute
value from applying a relative offset. Define all-or-nothing validation,
inheritance/reset, undo and behavior when a selected Zone disappears. Persistent
groups would be a separate decision; temporary selection need not add file
format or runtime complexity.

EOS Preset Links are less directly transferable. WaveX already has Track
layering and two oscillator maps. Linked Instruments would introduce dependency
ownership, missing references, recursive cycles, gain accumulation and voice
admission questions. Prefer the existing structures until a concrete sound
requires another level; a shared sample reference does not imply shared mutable
Instrument settings.

### Modulation as a small control-processing system

Cords suggest that expressiveness can come from transforming control signals,
not only adding oscillators or filters. Existing WaveX routes already carry
depth, curve and polarity information. First consider useful transformations
within that model; for example, shaping velocity or smoothing a controller.

Lag, sums, switches and modulation of another route's depth would go further:
they create dependencies and sometimes state between routes. Before adopting
them, define typed inputs/outputs, units/ranges, per-note versus global state,
reset behavior, evaluation order and a policy for cycles. Keep storage and work
bounded and prepare the evaluation plan outside the callback. Adding rows to
the current matrix alone does not solve those semantics.

Clock-derived modulation is musically useful for rhythmic sample/envelope
retriggering, but “tempo-synced” has two meanings: matching a rate and aligning
events to a clock phase. A future design must specify start/stop/seek,
tempo changes, retrigger ordering and note release on the Daisy musical clock.
UI timers must not become a second clock authority. More filter types, chorus
and six-stage envelopes remain separate DSP proposals with measured cost;
chorus cannot be treated as free width if it allocates additional renderers.

### Loading, memory and recording workflow

The two-RAM distinction is valuable even though WaveX has a different memory
layout. **PCM capacity, metadata capacity and render capacity are separate
admission limits.** A load can fit its samples and still exceed Zone/registry
slots or preparation buffers; a playable layered sound can exhaust channels.
Report the actual constrained resource instead of a generic “memory full”.
Do not copy EOS's user-adjustable RAM split into WaveX.

SoundSprint illustrates a desirable outcome: prepare another sound while the
current music continues. Reuse WaveX's cooperative loader, shared Sample Pool
and admission model. Stronger transactional replacement would require room for
both working sets, cancellation/failure cleanup and an atomic publication point;
the current loader does not guarantee restoration after every late I/O failure.
Keep unrelated Tracks playing and make the edited Track's replacement policy
explicit. Reserved preset numbers and hidden temporary Tracks are unnecessary.

Browsing inside a saved Bank/Project is also useful inspiration. Preserve typed
actions such as “recall Instrument into Track” versus “open Project”; EOS's
whole-RAM Bank corresponds more closely to WaveX Project than WaveX Bank.
Container inspection or selective Project import would need its own supported
codec/index and dependency admission, not an assumption that the file is a
directory or a new competing browser.

Recording followed by **Place Sample** offers a practical workflow: capture,
name, audition, then explicitly assign the completed asset to a selected
Track/oscillator/Zone with root and key/velocity ranges visible. Capture must
not silently replace an occupied map. Sample management, mapping and sound
editing can remain adjacent while their ownership stays distinct.

EOS's waveform Undo reinforces budgeting for render scratch and the previous
asset as part of offline editing. WaveX should publish a new rendered asset
and retain old references as needed, rather than rewrite shared PCM in place.
An exotic processing catalog is less valuable initially than reliable preview,
cancel, undo and save behavior.

## EOS Cords: further modulation lessons

The detailed Cord examples supplied on 2026-09-17 suggest a bounded control
graph with named signals, processors and addressable route depths. This remains
an exploratory WaveX direction, not an implementation specification or an EOS
emulation plan.

### Manual checks and example corrections

The [E-mu EOS 4.0 manual](https://www.deepsonic.ch/deep/docs_manuals/e-mu_eos_4.0_manual.pdf)
supports these specific observations (printed pages 262–263, 350–353):

- Amp envelope/amplifier and keyboard/pitch are permanent connections;
  other routings use Cords, including modulation of another Cord's amount.
- Lower-numbered Cords process realtime events first. Row order can therefore
  affect a dependent patch, rather than being merely visual organization.
- The `<` diagram is a subtractive source mapping; it should not be treated as
  a generic synonym for a concave response curve.
- Lag feeding LFO rate gradually changes speed. It does not by itself fade
  vibrato depth from silence.
- Sample retrigger uses a falling zero crossing; the flip-flop processor
  instead toggles when its input becomes positive. Trigger ports cannot all
  inherit one edge rule.

The supplied three-sources/two-destinations mixer example needs **five Cords**:
three into Sum, two out. Direct wiring needs six. Shared mixing uses `N + M`
routes instead of `N × M`, and is equivalent only when each destination wants
the same mixture, scaled as a whole. Consistent reuse matters more than an
assumed saving for every patch.

### Model signal meaning before adding destinations

For a future WaveX design, distinguish these contracts explicitly:

| Element | Proposed information that must be defined |
|---|---|
| Source | Identity, scope, range/units, update event or rate, initial value |
| Route | Stable identity, source transform, destination port, signed base depth and optional depth control |
| Processor | Type, input accumulation, state owner, reset policy and bounded evaluation cost |
| Destination | Continuous value, event-time value or trigger; combination, limits and application boundary |

Key/strike velocity and per-note random values can be latched at note-on.
Release velocity becomes available at note-off, with an explicit fallback when
the input supplies none. A proposed held-key Gate needs both press and release
transitions; sustain-held sound and a physically held key are distinct states.
Do not copy a blanket “all keyboard sources run once at strike” classification
into WaveX. Envelopes, LFOs and MIDI controllers need their own update policies,
and channel/Track controllers must not acquire per-voice ownership accidentally.

Separate **polarity/remapping**, **response curve** and **signed route depth**.
Define their order and show a useful range preview. A positive amount need not
increase perceived brightness or lengthen attack: the result depends on source
mapping and destination units. Envelope rate and envelope duration describe
speed in opposite ways; WaveX's time-valued controls must stay unambiguous.

### Depth control and reusable mixes

Wheel-controlled vibrato is a good acceptance example for a future route-depth
feature: `LFO → pitch`, with wheel controlling that route's effective depth.
Define additive depth modulation versus multiplication explicitly; multiplying
a zero base by a controller stays zero. Show base and effective depth, clamp
them deliberately, and specify how locks, live edits and Apply/Revert compose.
Route references must survive display reordering and handle deletion without
silently addressing a different row.

For delayed vibrato, smooth the depth controller (or use the existing LFO
delay/fade where it meets the need). Keep `Lag → LFO rate` as a separate
speed-ramp example. Shared mixes should expose their summed value and bounded
headroom; independent destination depths must remain adjustable.

Convenience controls such as Filter Env Amount should edit the authoritative
route rather than add a hidden parallel connection. Preserve WaveX's amp
envelope ownership and existing matrix semantics; useful default routes can
be explicit preset data without making every new Instrument silent or inert.

### Evaluation and trigger boundaries

A candidate WaveX graph would be validated and ordered in the foreground,
then published as one immutable bounded plan. Prefer dependency order to a
hidden reliance on displayed row order. Reject cycles initially, or introduce
explicit delayed feedback only through a later design; never let an accidental
cycle cause unbounded evaluation. Lag/flip-flop/edge-detector state needs
defined initialization and retirement on note start, retrigger, stealing,
Instrument replacement and graph edits.

Keep continuous modulation separate from sample-start evaluation and trigger
events. For each event port, define the edge, threshold, equality-at-zero,
initial-state and repeated-event rules, plus sign inversion and noise behavior.
Sample retrigger must specify which oscillator restarts, whether envelopes
restart, stereo cursor coherence and click handling. Clock-driven events need
musical-clock timing; sampling a square wave once per control block can miss
edges and does not establish sample-offset accuracy.

Realtime crossfades also need admission for every participating layer, including
currently silent ones, or an explicit next-note policy. Loop movement needs
valid bounds and residency rules; a modulation route does not grant arbitrary
PCM access. These are callback-capacity and ownership decisions before they
are additions to a destination menu.

## Comparing the useful ideas

| Question | ASR-10 lesson | E4 / EOS lesson | WaveX direction |
|---|---|---|---|
| Reach the right control | Stable mode/page addresses | Mapping and dynamic processing are separate views | Shallow navigation with visible object and edit scope |
| Shape a multisample sound | Layers and per-WaveSample synthesis | Shared synthesis definition across a sample map | Instrument defaults with explicit Zone overrides; consider batch edits |
| Perform variations | Immediate Patch Select masks | Controller windows, Cords and clock-derived modulation | Resident variation and bounded modulation proposals, with distinct ownership |
| Reuse content | Instruments collected in a machine Bank | Preset links and selective object loading | Shared Sample Pool, Track-owned sounds, typed Bank/Project actions |
| Stay predictable | Playback versus structural commands | Separate assets, preset metadata and costly processing | Explicit admission, coherent handoffs and offline render transactions |

The strongest E4 additions to the ASR comparison are **group edit scope,
shared sound definitions across maps, and resource-aware preparation**. These
fit WaveX more directly than copying the EOS hierarchy or expanding the DSP
catalog. Control processors are promising, but require a fuller design than
another source/destination enum value.

## Workflow coverage and remaining design gaps

Assessment requested 2026-09-17: the references suggest concrete improvements
outside synthesis. The table distinguishes existing plans from additional
product decisions. It is based on the canonical roadmap and linked feature
documents, not a complete implementation audit; a menu or codec alone does not
establish a finished workflow. This is design rationale, not another task list.

| Area | Already covered or planned | Useful gap to define |
|---|---|---|
| System settings | Settings has Display/Storage/MIDI/System/Calibrate surfaces; settings persistence is Phase 5 | One settings inventory stating owner, default, persistence location, reset scope and whether a change is live or requires restart. Distinguish device preferences from Project musical settings and transient state. |
| Wave editing | Marker/fade/waveform foundations; Phase 1.5 sidecar persistence, stereo snap and loop work; Phase 4 render pipeline and DSP catalog | A complete select-region → audition → edit → compare → undo → save workflow, with clear sample-wide versus Zone-only scope. Define clipboard/cut/copy/paste behavior if adopted, rather than assuming a waveform view implies it. |
| Sample management | Shared Pool, resident assignment and bounded maps; unload/rename/delete/reorder are already backlogged | Show file versus resident asset, users of the sample, edited state and resource cost. Separate unassign, unload RAM and delete file; settle duplicate/replace/rename effects on references. |
| Storage and Projects | Pattern/Instrument saves, Project codec/file-job foundations, planned session capture/restore, Bank Manager, later save-with-samples and USB import | Define portable collection, missing-asset repair, Save versus Save As, dirty-state handling, recovery and backup scope. Show which dependencies must succeed before a Project save is complete. |
| Sequencer and Song | Step grid, notes/velocity/locks and Pattern saves; planned Pattern management, Song arrangement, melodic lanes, live record and clock output | Specify count-in/metronome, replace/overdub/erase, bounded take undo, copy/duplicate/clear scope, and audible transition rules. Settle gates/ties before relying on recording or Song playback. |
| Recording and resampling | Phase 2.5 draft already describes threshold/pre-roll, meters, audition/save/discard, assign and master resampling | Complete the take lifecycle: failed-save retry, naming, admission, monitor/resample tap behavior and optional bar-aligned capture. Revise older draft details against current Pool, mixer and persistence ownership before implementation. |

### Playback head in waveform views

**Requested 2026-09-17.** The initial sample-scoped implementation is described
in [Waveform playback head](features/waveform-playback-head.md); physical
acceptance is open in HV-010. The requirements below also cover future reverse
playback and Track/Zone-specific contexts. Waveform views should
show a moving vertical playback-head line while the displayed sound is playing,
including browser audition and Sample editing. Use the same behavior wherever
that waveform is displayed. Keep the line visually distinct from trim, loop
and selection markers; stereo channels share one aligned head.

The head represents the source frame currently being rendered, mapped through
the displayed zoom/scroll window. It must follow playback rate, reverse,
retriggers and loop wraps; elapsed wall time or the SD read-ahead position is
not a sufficient source. Hide the line when the position is outside the visible
window, and clear it when playback ends, stops, is stolen or changes asset.

For polyphonic playback, the proposed initial rule is one head for the newest
active voice matching the displayed Track/Zone/sample context, falling back to
the next matching voice when it ends. Browser audition follows its own preview
instance. The current sample-only views use the newest matching sample voice,
with matching streaming audition taking precedence. Request identity, sample
identity and generation must prevent delayed telemetry from moving the head for
a newly selected sound.

Publish bounded playback snapshots from the engine and deliver coalesced,
rate-limited telemetry through the existing link. Render the overlay on the UI
task without refetching waveform data for each movement. The audio callback
must never wait for the display or send link traffic. Set the update cadence
from link and panel measurements; any UI interpolation must reconcile to the
engine at loop/retrigger/stop boundaries and expire stale state.

Acceptance should cover audition and played notes, zoom/scroll, stereo, pitched
and reverse playback, loops, overlapping voices, stop/steal, asset changes and
stale messages. Verify visible tracking on the panel under link traffic and
maximum supported audio load, with no audio underruns. This is a Phase 2
waveform workflow follow-up, not evidence that the current phase gate passed.

### Settings need ownership before more menu entries

A useful candidate split is:

- **Device preferences:** display behavior, control calibration, startup
  preference and physical MIDI-port defaults. Resetting preferences must not
  erase user Projects or samples; calibration reset should be separately scoped.
- **Project settings:** musical tempo/clock policy, Track MIDI routing, mixer
  state and the active tuning specified by the tuning design. Decide explicitly
  which device defaults seed a new Project and which Project fields override
  them on recall.
- **Transient state:** held notes, active previews, pending jobs and temporary
  selections. Do not accidentally restore these as sounding activity at boot.

Candidate settings include clock source/send/receive, MIDI thru/echo policy,
controller assignments, velocity response, default record/count-in behavior
and startup choice (empty, template or last Project). These are inventory
candidates, not claims that every control is absent. Each needs an owner and
failure behavior; failure to reopen a last Project should still permit clean
startup. A global Panic action should have an explicit note/transport scope.

### Sample and storage actions must state their reach

“Remove” is too ambiguous. Unassign removes a Zone reference; unload releases
eligible residency; delete changes storage and may break saved content.
Show known references and admit when references in unopened Projects have not
been indexed. An “unused” RAM sample is not proof that its file is unused.

Likewise, changing a Zone's loop override differs from changing shared sample
metadata or rendering new PCM. Prefer a new asset for destructive edits and
explicit reassignment, with audition and undo retaining the source. Define
whether Duplicate copies metadata, PCM or both.

A portable Project should collect the required Instrument snapshots and sample
dependencies, deduplicate them under a defined identity policy, and write
references relative to the collected set. Missing-file repair should validate
the replacement's format and musical metadata, not accept a matching basename
blindly. Save-with-samples, backup and recovery are different operations; a
checked temporary-file rename does not prove arbitrary power-loss durability.

### Sequencing needs predictable musical transactions

The high-value questions are what changes now, what changes at the next step
or bar, and what survives Stop. Pattern selection should expose active and
queued state; Song edits need a rule for the currently playing entry. A stopped
transport must clean up its own pending notes without ambiguously silencing
unrelated live playing.

For editing, start with a bounded undo unit such as the last edit or recording
pass. Define whether copying a step/row copies notes, locks and hidden steps,
and how length changes affect hidden content. External MIDI note sequencing
and SMF interchange are later candidates, not substitutes for finishing the
internal record/arrange/save workflow.

### Priority within the existing roadmap

The strongest next outcomes are **a session that reloads faithfully**, **a
sample edit that survives reboot**, **clear asset-management actions**, and
**a record/arrange workflow with dependable note lifetimes**. They fit the
existing Phase 1.5/2/2.5 dependencies; they do not authorize skipping the
current hardware gates. Complete capture and basic editing before expanding
the Phase 4 DSP catalog. General settings polish, portable libraries and
performance conveniences retain their existing later placement unless a
current gate requires a specific piece.

## Vintage sampler grit

**Requested 2026-09-17; feature direction, not implemented.** The goal is
early-sampler character on one-shots that can be layered into Instruments.
Offline asset preparation belongs with Phase 4, after the basic render,
audition and save pipeline. The engine remains at **48 kHz**; the requested live
playback mode simulates a separate sample clock for each voice. It is a musical approximation, not a claim to
reproduce a particular converter, SSM circuit or historical machine.

The [fixed-48 kHz math proposal](features/vintage-sampler-math.md) develops
fractional playback timing, zero-preserving companding and reconstruction,
with reproducible numerical checks and primary research sources.

### Desired sound and signal order

```text
Source + input trim
  → capture pre-filter / controllable anti-aliasing
  → sample-rate reduction (27,700 Hz or 22,050 Hz)
  → 8-bit μ-law encode/decode or explicitly named companded quantization
  → rate-based playback / reconstruction
  → low-pass (roughly 8–12 kHz, 12 or 24 dB/oct, mild resonance)
  → optional gentle saturation
  → output trim
```

The two requested rate choices are starting presets, not hardware-emulation
claims. Quantization must be companded; linear 8-bit is a separate optional
comparison, not an interchangeable implementation. An encode/decode round
trip can bake the quantization into PCM16, preserving the current playback
format. It does not provide 8-bit storage savings. Distinguish a validated
G.711 μ-law mapping from a tunable compressor → quantizer → expander model;
label the latter as companded reduction rather than bit-exact G.711. See the
[ITU G.711 reference](https://www.itu.int/rec/T-REC-G.711/en).

Pre-filtering, reconstruction and the final musical filter have different
jobs. The [Decimort 2 manual](https://d16.pl/pub/manuals/Decimort%202-manual-gb.pdf)
describes separate pre-resampling alias control and post-resampling image
filtering. Adopt that distinction: filtering afterward cannot remove folded
frequencies selectively once they overlap the wanted signal. Do not silently
use a pristine rate converter that removes the intended character.

Pitch changes use playback rate, changing duration as well as pitch. No
time-stretch or pitch-preserving correction belongs in this mode. Aliasing
may be intentional when pitching upward, but its amount depends on the source,
pitch and reconstruction algorithm; a lower source rate alone does not specify
the sound of a historical playback converter.

The final low-pass is an SSM-inspired tonal control, not an SSM model. Its
cutoff must be valid at its processing rate: a 22.05 kHz stage cannot implement
a 12 kHz cutoff because its Nyquist frequency is 11.025 kHz. Processing the
post-filter at WaveX's output rate can accommodate the requested range.
Saturation follows that filter. Existing pre-filter drive is not an equivalent
substitute, and “tape/transformer” describes the desired gentle character until
a specific model is designed and evaluated.

### Offline rendering versus playable source

Two products are useful, but they must not be presented as identical:

| Output | Behavior and limitation |
|---|---|
| **Printed one-shot** | Run the complete chain offline at a chosen playback-rate pitch, then save PCM16 at the engine rate. Filter and saturation follow the generated playback artifacts. Later transposition also shifts those baked stages; it does not rerun the modeled playback chain. |
| **Reduced-rate source** | Save companded/decompanded PCM16 at its actual reduced sample rate, preserving root/tuning and remapped markers. Normal rate-based playback remains possible. To preserve the full chain across live pitches, post-playback filtering and post-filter saturation need an explicit runtime path. Baking them into the source does not filter artifacts generated later. |

The user's fixed-48 kHz clarification makes the reduced-rate source plus live
virtual-clock playback the main target. Offline preparation supplies decoded
PCM16 at the capture rate; a voice advances by
`capture_rate / 48000 * 2^(semitones / 12)` source frames per output frame.
Reconstruction must account for fractional transitions and clocks above 48 kHz.
Printed one-shots remain an optional output. Any runtime reconstruction or
post-filter saturation addition needs its own measured callback gate and
independent scope; this research does not advance the current roadmap phase.

### Reference compander and prototype limits

The user supplied Python and C prototypes on 2026-09-17. Retain their smooth
compander as a reference equation for normalized input, with `mu = 255`:

```text
F(x)    = sign(x) * log1p(mu * abs(x)) / log1p(mu)
Finv(y) = sign(y) * expm1(abs(y) * log1p(mu)) / mu
output  = Finv(Q8(F(clamp(input, -1, 1))))
```

The continuous equation plus uniform quantization is a creative companded
reducer, not by itself the normative G.711 byte codec. A byte-code mode needs
comparison against the [ITU reference implementation](https://www.itu.int/rec/T-REC-G.711/en),
including sign/segment boundaries and zero codes. The claim that one path is
closer to the EII, and the historical explanation for lower recording rates,
remain unverified; 22.05 and 13.85 kHz can be creative rate presets without
making those claims.

Resolve these prototype details before implementation:

- **Quantizer and silence:** the supplied endpoint-inclusive
  `q = round((y + 1) * 127.5); yq = q / 127.5 - 1` has 256 levels but no
  exact zero reconstruction value. At zero input it selects code 128 and
  expands to approximately `+0.00008621` (−81.29 dBFS), a DC offset, not
  random noise. Define zero handling, rounding, clipping and optional dither
  deliberately rather than adopting the “mid-riser” label as a specification.
  [NumPy round](https://numpy.org/doc/stable/reference/generated/numpy.round.html)
  uses ties-to-even; C `lround` uses halfway-away-from-zero, so the supplied
  languages need a shared rounding rule for matching boundary vectors.
- **Capture rate versus pitch:** downsample/decode/upsample at consistent
  rates preserves nominal pitch and duration. Changing the C hold clock
  changes degradation, not stored-sample playback pitch. A true pitch control
  advances a stored source at a different rate and changes its duration;
  preserve that separation in the render recipe and UI.
- **Two different reconstruction models:** Python
  [resample_poly](https://docs.scipy.org/doc/scipy/reference/generated/scipy.signal.resample_poly.html)
  includes a low-pass FIR. Its down/up conversion is a filtered reference,
  not an implementation of a zero-order hold or freely adjustable foldover.
  The supplied C loop reads the current filtered host sample at each tick
  and holds it; it does not perform the claimed linear interpolation. At
  48 kHz its ticks fall on one- or two-host-sample intervals. Define fractional
  capture timing and reconstruction explicitly before comparing algorithms.
- **Filter meaning and bounds:** the Python
  [sosfiltfilt](https://docs.scipy.org/doc/scipy/reference/generated/scipy.signal.sosfiltfilt.html)
  call runs forward and backward, giving zero phase and the squared magnitude
  response rather than one causal eight-pole analog-like pass. It also requires
  enough samples for padding. A fixed 12.5 kHz digital cutoff is invalid for
  22.05/24 kHz input. Derive the capture cutoff from the selected capture rate
  (the suggested `0.45 × rate` is a starting point) and validate against input
  Nyquist. Short/empty clips need an explicit policy.
- **Prototype completeness:** the Python CLI also needs `soundfile`; validate
  rates, bit count, finite input and channel shape, and make output subtype,
  length, tails and metadata explicit. Its `bits >= 16` branch bypasses
  quantization rather than providing a general bit-depth control. Neither
  supplied processor implements the final resonant filter and optional
  post-filter saturation from the complete requested chain.
- **Embedded suitability:** fix the C `MU` guard (it currently tests
  `M_LN256`), validate rates and specify phase units. No heap allocation alone
  does not establish callback safety: repeated double-precision logarithm/
  exponential calls and the rate-dependent loop need bounds and measurements.
  Keep companded asset preparation offline; evaluate library primitives and
  bounded reconstruction before implementing the requested runtime playback.

These are analytical checks of the supplied snippets and API documentation.
The [math experiment](features/vintage-sampler-math.md#numerical-evidence-and-remaining-work)
now checks selected quantizer and clock properties; it is not a complete
renderer, G.711 codec or device effect. Further cases should include tiny
signed inputs, quantizer ties, clipping, short clips, supported rates, stereo
and full-chain behavior across different processing chunk sizes.

### Controls, ownership and acceptance

Suggested controls: rate preset, input level, companding model, pre-filter
amount/cutoff, reconstruction character, render pitch, post-filter cutoff/
slope/resonance, saturation and output level. Preserve level through the
compander deliberately; automatic normalization must not erase the effect of
input level on quantization. Start with deterministic presets; jitter, noise
and codec-loss effects are separate additions.

The render recipe owns these settings and its source reference; the original
asset stays unchanged. A completed result becomes a new Sample Pool asset,
assigned explicitly to an existing Zone or oscillator for layering. Use the
shared offline job lifecycle, admission, cancel, A/B audition and new-copy save
behavior. Keep stereo timing coherent and rescale sample/loop markers when
rate or duration changes. Do not introduce another saved Instrument hierarchy
or a runtime μ-law decoder solely for this effect.

Acceptance should compare transient one-shots and quiet decays at matched
listening levels, plus tones/sweeps for quantization, foldover and reconstruction
behavior. Check both rates, upward/downward rate pitch, filter/saturation order,
stereo coherence, silence/clipping, save/reload and cancel. Listen while a
Pattern plays and measure render servicing and underruns on hardware before
claiming uninterrupted operation.

Decimort, Lossy and Redux are user-supplied listening references, not required
dependencies or interchangeable algorithms. The requested companded sampler
chain defines this feature; other loss/degradation styles can be evaluated
separately.

## Fit with WaveX's roadmap

Navigation/context ideas can inform the already scheduled Instrument and
Project work in Phases 2/2.5. Offline command presentation belongs with Phase 4;
performance recall belongs with Phase 5 Scenes/macros. New articulation groups,
Instrument variations, envelope models, control processors and source modulation remain
unscheduled until promoted through a focused feature design.

The E4 recording/placement and load-admission lessons can inform existing
Phase 2/2.5 work; group editing and selective container import are unscheduled
workflow candidates. Clock-triggered modulation must follow the existing
sequencer/MIDI timing contracts and its own capacity measurement. These notes
do not add requirements to the current phase gate.

No firmware behavior, persistence contract or phase gate changes here. Compare
future reference instruments against the same ownership and interaction
questions, then update the relevant canonical feature document if an idea is
accepted. The roadmap remains the sole task list.

## Related

- [Project principles](project-principles.md)
- [System architecture](architecture.md)
- [Project, menus, mixing and voice channels](features/project-menu-and-voice-model.md)
- [Track and Instrument model](features/track-and-patch-model.md)
- [As-built Instrument model](features/instrument-model.md)
- [UI navigation and context](ui-architecture.md)
- [Modulation and parameter locks](features/param-locks-and-modulation.md)
- [Offline sample editing](features/offline-sample-editing.md)
- [Sampling and recording](features/sampling-and-recording.md)
- [Project persistence](features/project-persistence.md)
- [Melodic sequencing](features/melodic-sequencing.md)
- [Tuning and scales](features/tuning-and-scales.md)
