# Parameter Locks & Modulation Matrix — Design

**Status**: Voice-scoped parameter locks, touch editing and live Play-control
capture are implemented. The saved Instrument matrix offers oscillator mix,
OSC1/OSC2 pitch, cutoff, resonance, gain, pan and both per-voice LFO rates.
CC1/channel pressure, three envelopes and two per-voice LFOs are available as
sources. Callback capacity and physical acceptance remain open; see
[the hardware checklist](../hardware-validation.md). Session-global LFO editing,
held-step encoder locks and eviction notices are implemented. Wavetable positions
and analog/group locks remain targets.
**Dependencies**: sequencer step scheduler (Phase 2), `instrument-model.md` (matrix slots are instrument-scoped), voice manager (done). **Revised 2026-09-04**: the two-oscillator Instrument (`track-and-patch-model.md` §3.1) fixes the source/destination set this matrix serves — three envelopes, two per-voice LFOs, one global LFO, oscillator and wavetable-position destinations — appended to the enums below, never renumbered.
**Lineage**: two ancestries deliberately fused — Elektron parameter locks (per-step sound design) and the E-mu EIII **realtime controls matrix** (velocity/wheel/pedal → pitch, filter, level, LFO amount, attack — routed, not hardwired).

---

## 1. Parameter identity and mappings

The wire contract is `ControlParameter` in
[protocol.h](../../firmware/shared/spi_protocol/protocol.h). Existing PAN (0x08)
and PITCH (0x09) retain their identities. The earlier proposed duplicate ids are
withdrawn. SAMPLE_START, LOOP_START and GAIN occupy their reserved ids; macros
and analog/group controls remain reserved.

All locks carry a uint16 value. Cutoff maps exponentially from 20 Hz to 20 kHz;
resonance/sustain/pan map from zero to one; ADSR times map from 1 to 2001 ms.
Pitch replaces the live Track offset over -24 to +24 semitones while retaining
zone tuning. Gain scales the resolved zone gain, with 32768 meaning unity.
Start and loop start map within their original resolved ranges, leaving at least
two frames. Loop start does nothing when looping is disabled. Other voice
controls compose with the existing post-trigger modulation.

## Touch editing (as built)

Sequencer → Shift → Locks displays four slots above the grid. Drag Slot,
Parameter and Value to choose an override; dragging an occupied slot's value
edits it directly. Tapping a step selects it without toggling its note while in
Locks view. Clear lock removes only the selected override. Grid returns to the
ordinary step editor. An asterisk marks steps containing stored locks.

Slot replacement is one atomic pattern operation, rejects duplicate parameters,
and waits for backend readback. Live Instrument edits still update unlocked
fields on held voices; locked fields retain their trigger values. A subsequent
unlocked step resolves the Instrument normally. Master volume, global LFO,
macros and analog/group parameters are not offered as voice locks. Unsupported
ids in files are preserved but ignored by the renderer.

### Held-step encoder editing

A short tap toggles a grid step on release. A long press selects the step and
opens Locks without toggling it. While held, four encoder bindings edit the four
visible lock slots; an empty slot chooses the first unused supported parameter.
Release, lost press, navigation, Track changes, disconnect or a replacement
Pattern epoch cancels the hold. Explicit-step writes use the existing scoped
Pattern edit contract, so a delayed operation cannot edit a replacement Pattern.
The lock editor remains available after release. Physical encoder feel and
press-to-panel latency remain hardware checks.

## 2. Parameter locks

Already in the pattern model (`sequencer.md` §3): `param_locks[≤4]{param_id, value}` per step. This doc pins the application path:

- At step-fire time the scheduler resolves the step's zone/notes into `VoiceTriggerParams` (via `instrument-model.md` §3) and then applies each lock **to the trigger params** for voice-scoped ids (cutoff, ADSR, pitch offset via `pitch_ratio_mul`, pan, gain via `gain_mul`, sample start as region offset). No global state is touched — a p-lock on step 5 cannot bleed into a live-played note. Host-testable pure function: `ApplyParamLocks(VoiceTriggerParams&, const ParamLock*, n)`.
- **Target only:** locks on *track/group-scoped* ids (PARAM_ANALOG_CUTOFF, sends later) would stage a control-tick value that reverts to base at the next step boundary of that track (Elektron behavior: the lock lasts one step). Implemented as `{value, revert_at_tick}` pairs in the track state.
- **Live p-lock recording (as built):** on Step Notes select Live rec and Play,
  then move a Play-page cutoff/resonance/ADSR control. PAN/PITCH control messages
  also record. Only the armed recording Track captures; other Tracks still
  receive their ordinary live sound edits. The callback writes into the nominal
  step containing its current Pattern time, independent of note quantization,
  swing and micro-offset. A knob alone never enables a step or makes it melodic.
  Repeated parameters update their existing slot; new parameters use a free slot
  or evict slot 1 and shift the remaining locks left. Pattern/Project saves retain
  the result. Master/global controls and typed Instrument editor operations do
  not record locks. Eviction posts a three-second header notice identifying the
  Track, step and replaced parameter; bursts retain the latest notice and count.
- Motion arrives through the existing bounded sequencer command queue, tagged
  with the callback-published Pattern epoch. A stale Pattern, Song playback,
  stopped transport or another input mode rejects capture. Queue overflow drops
  capture with a foreground diagnostic; it never blocks audio. The callback owns
  the pending Pattern, marks touched lock slots and publishes those records at
  the same safe step boundary as ordinary edits. Unchanged values do not advance
  the revision. Step Notes gives confirmed Pattern-update feedback; Locks view
  shows the stored overrides and grid badges.

## 3. Modulation matrix (instrument-scoped)

Stored in `Instrument` (`instrument-model.md` §2), 8 slots:

```cpp
struct ModSlot {
    uint8_t source;   // ModSource, see mod_matrix.hpp and protocol.h
    uint8_t dest;     // ModDest, see mod_matrix.hpp and protocol.h
    int16_t depth;    // ±32767 → ±100%
    uint8_t curve;    // 0=linear, 1=exponential, 2=S-curve polynomial
    uint8_t flags;    // bit0: unipolar source remap to bipolar
};
```

`source` and `dest` use the append-only `ModSource` and `ModDest` enums in
`firmware/daisy/src/audio/mod_matrix.hpp`, mirrored as raw bytes in
`firmware/shared/spi_protocol/protocol.h`. The five fields are source,
destination, signed depth, curve and flags. Current destinations include
cutoff, gain, pitch, pan, resonance, OSC1_PITCH, OSC2_PITCH, OSC_MIX,
LFO1_RATE and LFO2_RATE. Wavetable-position destinations remain planned.

**Evaluation model — control-rate, never per-sample**: once per control block (nominally 1 ms), each active voice sums its instrument routes before applying destination
clamps. Cutoff and resonance are handed to the existing SVF through combined
`VoiceFilter::SetParameters` once per block; gain/pan are block-constant
multipliers and pitch updates the increment once per block. Callback cost is a
measured gate, not an assumed constant; any future destination or callback
change requires a fresh DWT capacity run.

Resonance modulation uses a signed normalized offset. A full-depth source 1
produces `+1`; routes are summed before the offset is clamped to `[-1, 1]`,
then added to the base resonance and clamped to `[0, 1]`. A parameter lock may
set the base resonance, and live base edits honor that lock; clearing the last
resonance route restores the unmodulated base, while Revert restores the prior
matrix and its modulation. A stolen voice resets the modulation state. This
extends the bounded `Voice` block-modulation handoff without adding a new
transport or per-sample work.

The control tick writes the bounded `SetBlockModulation(const
ModDestinations&)` prepared snapshot, including the resonance offset; the
render path consumes it for voice-block tuning.

Oscillator pitch routes use the source oscillator identity: OSC1_PITCH and
OSC2_PITCH address their corresponding oscillator, including an Oscillator 2
primary cursor when that source owns the voice. A full-depth source 1 raises
pitch by 2 semitones at +100% depth and lowers it by 2 semitones at -100%;
routes sum before the pitch scale is exponentiated and composed
with common pitch, note/zone/live tuning and pitch locks. Sync and FM remain separate future work.

Oscillator mix is a signed normalized offset: routes sum, clamp to `[-1, 1]`,
then add to the Instrument's base mix and clamp to `[0, 1]`. Source oscillator
identity also applies when OSC2 is the primary cursor. Separate unmixed levels
preserve zone/sample gain and let a source muted by the base crossfade become
audible without a retrigger or division by zero. An oscillator level of zero
stays silent. Clearing the route restores the current base mix.

LFO1_RATE/LFO2_RATE address the two Instrument LFOs, with full depth spanning
±4 octaves (1/16–16 times base rate). Routes sum and clamp before conversion;
the effective rate stays within 0.01–100 Hz. Both Hz and tempo divisions can be
modulated, preserving phase, note age, delay and fade. Each block advances the
LFOs using the previous block's rate, evaluates the matrix, then publishes rates
for the next block. This one-block delay makes self/cross-LFO routing causal.
Clearing routes returns to the base rate on the next advance; retrigger resets
rate modulation to unity.

## 4. Filter and auxiliary envelopes (as built)

All three envelopes have saved Instrument settings. Env 1 drives amplitude;
Env 2 and Env 3 are block-rate matrix sources. The latter advance by the
active frame count and share note/release/choke ownership with Env 1, without
adding per-sample envelope processing. Live edits preserve held-note state;
the common Instrument preview and Apply/Revert controls retain their settings.

## 5. LFOs

Decided 2026-09-04 (`track-and-patch-model.md` §3.1, §9 item 16): **two
per-voice LFOs owned by the Instrument plus one engine-global LFO.** The
earlier "2 global + 1 per-voice" split is withdrawn; `SRC_LFO2` stays in the
enum as retired-but-reserved and reads 0.

- **1 global LFO** (control-tick, in `AudioEngine`, built as `SRC_LFO1`): sine/tri/saw/square/S&H, rate either Hz (0.01–100) or tempo-synced divisions (1/16 … 4 bars — needs the sequencer clock; follows the current internal/MIDI tempo). Phase-restart options: free, on-transport-start, on-any-note. Engine-global like a modular's LFO bank: performance-wide movement, not part of any Instrument.
- **2 per-voice LFOs** (`SRC_LFO_VOICE`, `SRC_LFO_VOICE2`): phase accumulator per voice per LFO, waveform/rate/delay/fade/retrigger and pitch-follow from the Instrument (`Instrument::lfo[2]`, saved in the `LFO1`/`LFO2` chunks), evaluated per block from a Q32 frame/beat epoch. Rates support Hz (0.01–100) or tempo divisions (1/16 … 4 bars, including 3/16); pitch-follow applies only to Hz. Retrigger at note-on with optional `delay_s` and `fade_s`. Their rates are mod destinations (`LFO1_RATE`, `LFO2_RATE`). The backend, typed transport and two-row eight-tile LFO page are implemented; edits use the common automatic-preview Apply/Revert path and WXI save retains the audible working copy. Live rate/wave/delay/fade edits preserve held-note LFO phase and age; gate/free admission policy applies to the next note.
- Why the Instrument owns them: an `.wxi` must sound the same on any Track and in any Project. Slot 3's wobble must not change because slot 5 loaded a new Instrument, and it must not depend on an engine setting the file does not carry.

## 6. Protocol

- Matrix/envelope edits now use the typed revisioned `MSG_INST_MOD_OP` and
  `MSG_INST_MOD_SYNC` messages in [the protocol](inter-mcu-protocol.md).
  Per-voice LFO snapshots use the typed revisioned `MSG_INST_LFO_OP`/`MSG_INST_LFO_SYNC` pair (0x6E/0x6F); global LFO GET/SET/RESET uses a separate session revision and callback snapshot.
- Live MIDI uses `MSG_MIDI_CC` (0x56) and `MSG_MIDI_PRESSURE` (0x8D).
  Both DIN and USB share the parser/forwarder. CC1 drives Mod Wheel and
  channel pressure drives Pressure in the Instrument Mod source selector.
  Values are 0–127, normalized to 0–1 by the Daisy; unsupported CCs are ignored.
  CC121 resets these two supported controller sources to zero.

### MIDI source ownership (as-built)

The foreground resolves each incoming MIDI channel to the current Track input
routing. Off receives nothing; Omni uses the latest matching event from any
channel. DIN and USB share the same channel namespace, as notes already do.
The two values are session-owned per Track, not saved into Instruments, Banks
or Projects. They affect all voices on that Track, including held/releasing
notes and subsequent live or sequenced triggers. A routing change resets the
Track's values; retiring/rebinding a Track resets them through the same
foreground stop boundary. A backend restart starts at zero.

Foreground updates publish a complete fixed sixteen-Track snapshot. The audio
callback acquires the newest immutable snapshot once per block; controller
bursts coalesce without occupying note or MIDI-clock queues. Intermediate
controller positions may be coalesced. Sample-save jobs continue accepting
expression; Project/Bank transactions retain their existing input freeze. Port loss does not synthesize a reset;
send CC121 or change the Track routing to clear a held controller value.
This is channel pressure, not polyphonic key pressure or MPE.

Message grammar and CC121 semantics follow the MIDI Association's
[message summary](https://midi.org/summary-of-midi-1-0-messages) and
[controller table](https://midi.org/midi-1-0-control-change-messages).
Physical port/latency and listening checks are tracked in HV-028.

- P-lock edit/record ops are `SEQ_PATTERN_OP` extensions (already reserved in `sequencer.md` §4).

## 7. UI (ESP32)

1. **Step hold + knob** = write p-lock (the core Elektron gesture); locked steps render with a corner badge; step hold shows current locks with per-lock clear.
2. **Mod page** (per instrument slot): 8 slot rows `source → dest, depth, curve`; encoder-driven. Live value bars per source remain a target (needs a coalesced `MSG_INST_STATUS` extension or piggyback on meter cadence — 10 Hz is plenty).
3. **LFO page**: expose the two Instrument-owned per-voice LFOs; the
   engine-global LFO stays performance-owned. The implemented page uses eight
   tiles in two rows and the common automatic-preview Apply/Revert path.
   Held-voice propagation for the implemented LFO controls is built; global
   LFO editing is available through Global LFO or Settings → Global LFO.

## 8. Test plan

- Host: `ApplyParamLocks` mapping tests per param id (value scaling, clamps); matrix evaluation golden tests (sources synthetic, verify per-block dest values incl. curves and unipolar/bipolar remap); env2/LFO block-rate progression; per-trigger random reproducibility with seeded RNG (`sequencer.md` §6 pattern).
- Host: revert-at-step-boundary for track-scoped locks (the "lock lasts one step" invariant).
- Hardware: DWT-measure control-tick cost with 8 voices × 8 slots + 3 LFOs active; budget < 10% of the tick. Audible: vibrato smoothness at block-rate pitch mod (sanity listen), filter-env sweep on the Stage A analog path via SRC_PARA_ENV → PARAM_ANALOG_CUTOFF.

## 9. Implementation status

- Voice-scoped lock application, individual-field live-edit protection, four-slot
  touch editing, single-slot clearing and pattern save/load are implemented.
  Host tests cover mapping, region bounds, unsupported ids, live-note isolation,
  slot identity and duplicate rejection. The two-board editor/recall HIL passed.
  Callback measurements are recorded separately in
  [callback-performance-log.md](../callback-performance-log.md).
- The block modulation surface and per-trigger velocity/note/random sources are
  implemented. Env 2/3 are block-rate sources and share note/release/choke
  lifecycle with Env 1. All three have saved Instrument settings and touch editing.
- The eight-row Instrument matrix and Instrument Mod editor are implemented.
  Foreground edits publish complete per-Track snapshots through mailboxes;
  the callback never reads a partially edited matrix.
- The backend and ESP32 now run two Instrument-owned per-voice LFOs with typed
  revisioned snapshots, WXI retention, append-only source ids and the two-row
  touch page. Held-voice propagation and the resonance destination are built;
  global LFO editing is session-owned and independent of Instrument Apply/Revert.
- MIDI CC1/channel-pressure source wiring and UI selection are implemented.
  Live Play-control capture, held-step encoder editing, session-global LFO
  controls and eviction notices are implemented. Analog/group lock lifetimes
  remain target design. Physical acceptance is tracked in HV-032.


### LFO rate controls

Implemented 2026-09-17: Sync is an Off/On control. Rate displays Hz when Off
and a musical duration when On, including 1/4 and 3/16. Unsynced rate spans
**0.01–100 Hz**, adjusted logarithmically (100 normal steps per decade; Shift
uses the existing fine-adjust divisor). Small changes retain float precision;
the display uses extra decimals at slow rates. Increasing Rate selects faster
cycles in both modes. One 0.01-Hz cycle takes 100 s.

Sync does not overwrite the independent Hz setting; turning it off restores
that rate. Enabling Sync from Off starts at 1/4; Rate then selects the duration.
The shared `audio/lfo_config.hpp` owns runtime bounds and division identities.
Existing wire/WXI IDs retain their meanings; 3/16 is appended. WXI still retains
the broader 0–1000-Hz storage domain for compatibility, while runtime and UI
adjustment clamp to 0.01–100. Both Instrument and global LFO runtimes use these
bounds; the session-global editor offers the same Hz and sync divisions.

Instrument LFOs follow the current internal or MIDI-followed tempo. Sync changes
rate only: held voices keep phase across rate/mode edits and transport/SPP
relocation. Gate restarts on note-on; Free takes the continuously advancing
engine frame/beat epoch at admission. SPP phase resetting is not implemented.
Modulation stays at control rate, including at 100 Hz; this is not a new
audio-rate modulation path. Audible quality and callback headroom are unverified
in [HV-015](../hardware-validation.md#hv-015--lfo-range-and-musical-rate-controls).

The bench console retains its existing raw commands: `RATE` uses millihertz
(10–100000), and `SYNC` uses the shared division ID (0 disables sync). The
visible Rate/Sync controls use the mode-aware behavior above.
