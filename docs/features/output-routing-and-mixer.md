# Output Routing & Mixer — Design

**Status**: Target design (unimplemented). Mixer v1 is Phase 2.5 (per-track control is core groovebox workflow); routing matrix is Phase 3/5 (needs Stage B hardware / send FX).
**Dependencies**: `instrument-model.md` (slots are the mixer's tracks), output sink seam (`architecture.md` §5.4, done), Stage B TDM path (Phase 3) for physical multi-out.
**Lineage**: E-mu presets routed to main/sub outputs per preset — the studio workflow was stems-per-instrument. Stage B's per-voice analog outs recreate that physically; the mixer here is the digital control layer over both stages.

---

## 1. Mixer v1 (Stage A — everything sums to SAI1 stereo)

Per instrument slot (= track), engine-side state applied as block-rate multipliers in the voice render sum:

```cpp
struct TrackMix {           // ×16, engine-global, control-tick applied
    float   gain = 1.0f;    // post-voice, pre-master; UI in dB (−inf..+6)
    float   pan_offset = 0.0f;  // −1..+1 added onto voice pan
    uint8_t mute = 0;       // soft mute: 5 ms gain ramp, not a hard cut (no clicks)
    uint8_t solo = 0;       // solo bus logic on the ESP32 side → emitted as mutes
};
+ master: float master_gain; (existing PARAM_VOLUME becomes explicitly master-scoped)
```

- Application point: `Voice` already renders with `gain/pan`; track gain/pan compose as block-constant multipliers via the same `SetBlockModulation` surface `param-locks-and-modulation.md` §3 adds — no new per-sample code.
- Solo is **UI-computed** (solo set → send mutes for everyone else): the engine stays a dumb mute/gain table; link-loss can't strand a hidden solo state on the Daisy.
- Mute ramps ride the slew engine (`scenes-and-performance.md` §3) — one mechanism.

## 2. Metering per track

Extend meter flow, not cadence: `MSG_MIX_METERS` (0x79, D→E) `{uint8_t peak[16]}` — per-track peak, log-mapped to a byte, coalesced at the existing 20–50 ms meter cadence, sent only while the mixer page is open (`MSG_MIX_OP` subscribe/unsubscribe op — don't burn link bandwidth for a hidden page). Master stereo meters stay on `MSG_METER_PUSH` unchanged.

Accumulation cost: per-voice |peak| max-tracking into its track's cell during render — one compare per block per voice, negligible.

## 3. Routing (Stage B and beyond)

- **Stage B**: zone `output_bus` (`instrument-model.md` §2) selects analog voice group (1+g) vs digital stereo (0). Voice-index = TDM-slot = CV-group invariants hold (`architecture.md` §5.4); a bus-1+g zone renders into the TDM slot of the voice playing it and its VCF/VCA CVs route to that group. Digital-stereo zones keep flowing to SAI1 — both paths concurrently is the intended mixed mode (e.g. drums analog, pads digital).
- **Send FX** (Phase 5 delay/reverb): `TrackMix` grows `send[2]`; sends tap post-track-gain into the stereo FX returns on the master bus. Reserved fields now (WXCF chunk + message reserve bytes) so the wire format doesn't bump.
- Aux *digital* output pairs beyond SAI1: no hardware exists — explicitly out of scope; noted so nobody designs against phantom jacks.

## 4. Protocol & persistence

`MSG_MIX_OP` (0x78, E→D): `{uint8_t op; uint8_t track; uint16_t value}` — ops SET_GAIN, SET_PAN, SET_MUTE, SET_MASTER, SUB_METERS, UNSUB_METERS. Small, idempotent, no bulk state (project load replays them). Round-trip + dispatch tests + doc rows, same commit. Mixer state persists in the project file (WXCF chunk, ESP-owned like scenes).

## 5. UI

**Mixer page**: 16 narrow channel strips (gain fader via touch drag + encoder fine, pan, mute/solo), master strip with existing stereo meters; per-track peak bars from 0x79. Follows `ui-architecture.md` deferred-update rules — meters land via the queued-update path, never direct LVGL writes from the comm task.

## 6. Tests & stages

Host: mute-ramp continuity (no discontinuity > step bound), solo-set → mute-set expansion (ESP-side pure function), gain dB↔linear mapping goldens, meter byte mapping monotonicity. Hardware: 16-track pattern, mute/solo punching during playback — no clicks (scope the output at mute edges); meter page on/off toggles link traffic as expected (packet counters).

Stages (one commit each): 1) `TrackMix` + engine application + host tests; 2) protocol 0x78/0x79 + tests + docs; 3) mixer page + solo logic; 4) meter subscribe path + bench click/soak test. (Stage B routing lands inside Phase 3's existing items; send FX inside Phase 5's.)
