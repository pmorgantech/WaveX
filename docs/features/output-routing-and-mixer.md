# Output Routing & Mixer — Design

**Status:** Mixer v1 controls and subscribed meters are implemented and
host/compile tested. Project retains selected-Track controls; Mixer provides
eight strips per page plus master, authoritative gain/pan/mute readback and
shared Sequencer Solo. Hardware touch, click, DWT and soak checks remain open
in [HV-006](../hardware-validation.md#hv-006--mixer-controls-and-master).
Stage B routing and send effects below remain target design. Mixer is Phase
2/2.5; routing follows the Phase 3/5 hardware and effects gates.
**Dependencies**: `instrument-model.md` (slots are the mixer's tracks), output sink seam (`architecture.md` §5.4, done), Stage B TDM path (Phase 3) for physical multi-out.
**Lineage**: E-mu presets routed to main/sub outputs per preset — the studio workflow was stems-per-instrument. Stage B's per-voice analog outs recreate that physically; the mixer here is the digital control layer over both stages.

---

## 1. Mixer v1 (Stage A — everything sums to SAI1 stereo)

Each Track has gain, pan/balance and manual mute targets in
[`TrackMix`](../../firmware/shared/audio/track_mix.hpp). The Daisy foreground
owns accepted targets; an immutable handoff updates the callback-owned mixer.
Solo remains a separate, temporary UI selection. The same handoff owns master
gain; `PARAM_VOLUME` aliases that target.

- Stereo-aware placement: Instrument and Track gains multiply. Instrument/zone
  pan, modulation and Track offset form one clamped position; mono uses linear
  pan and stereo uses balance with unity gain at center. No Track operation
  overwrites the saved Instrument trim. A stereo voice retains independent
  filter state for each side and shares its envelope and pitch timing.
- Application point: `Voice` already renders with `gain/pan`; track gain/pan compose as block-constant multipliers via the same `SetBlockModulation` surface `param-locks-and-modulation.md` §3 adds — no new per-sample code.
- Solo selection is UI-owned and sent as a separate mask. The handoff combines
  Solo exclusion with user mutes; manual mute wins. Clearing Solo restores
  current user mute targets, including edits made while soloed. Readback and
  Project storage use user mutes, never the temporary Solo exclusions.
- Track mute uses the existing 5 ms block-rate ramp in `TrackMixer`;
  Scene morphing remains future work.

Master gain is callback-owned and applied after audition and voice summation,
before the existing stereo meters. Both channels use the same sample gain.
Repeated identical controls do not restart its fixed 5 ms ramp; a new target
starts at the current gain. Legacy `PARAM_VOLUME` maps linear 0–1 to the same
foreground master target; mixer controls extend the range to +6 dB. The ramp
is fused into the existing output/meter traversal (no extra buffer or traversal).
Hardware click checks and DWT timing remain open in
[HV-006](../hardware-validation.md#hv-006--mixer-controls-and-master).

## 2. Metering per track

`MSG_MIX_METERS` reports the strongest post-Track-gain/pan/mute voice
contribution on either stereo side, before master gain. This is a Track
activity peak, not the phase-dependent sum of polyphonic voices or a bus clip
meter. The existing master stereo meters measure the actual final sum.

While subscribed, the callback holds each Track's peak across 40 ms windows,
so a short transient is retained until publication. Fixed-size snapshots cross
to the foreground, which performs the logarithmic byte mapping and sends at
most one packet per window. Queue pressure coalesces to the newest window;
no link or logarithmic work enters the callback. The per-sample peak update is
fused into the existing voice traversal; its DWT cost remains unmeasured.

The Mixer page renews its subscription once per second and unsubscribes on
exit. A three-second backend lease stops capture/sending after a lost exit
message or frontend reboot. The comm task caches whole meter snapshots under
a short lock; only the UI timer changes bars. Stale data clears after 200 ms,
and equal values do not invalidate the display.

## 3. Routing (Stage B and beyond)

- **Stage B**: zone `output_bus` (`instrument-model.md` §2) selects analog voice group (1+g) vs digital stereo (0). Voice-index = TDM-slot = CV-group invariants hold (`architecture.md` §5.4); a bus-1+g zone renders into the TDM slot of the voice playing it and its VCF/VCA CVs route to that group. Digital-stereo zones keep flowing to SAI1 — both paths concurrently is the intended mixed mode (e.g. drums analog, pads digital).
- **Send FX** (Phase 5 delay/reverb): `TrackMix` grows `send[2]`; sends tap post-track-gain into the stereo FX returns on the master bus. Reserved fields now (WXCF chunk + message reserve bytes) so the wire format doesn't bump.
- Aux *digital* output pairs beyond SAI1: no hardware exists — explicitly out of scope; noted so nobody designs against phantom jacks.

## 4. Protocol & persistence

The centralized [wire contract](inter-mcu-protocol.md) defines idempotent
controls, separate mute/Solo masks, subscription and correlated Track/master
readback. [Project persistence](project-persistence.md) includes Track mix and
master targets in its codec; device session capture/restore remains open.
Solo is transient and is not persisted as a mute set.

## 5. UI

**Mixer page (as built)**: eight Track strips per page plus master, with a
page switch covering all 16 Tracks. Faders commit on touch release; encoder
fine adjustment, pan/balance, manual mute and single-Track Solo are available.
Solo is shared with Sequencer and independent of manual mutes. Master stereo
meters remain in the header; each Track has a subscribed peak bar. Follows `ui-architecture.md` deferred-update rules — meters land via the queued-update path, never direct LVGL writes from the comm task.

## 6. Tests & stages

Host: mute-ramp continuity (no discontinuity > step bound), solo-set → mute-set expansion (ESP-side pure function), gain dB↔linear mapping goldens, meter byte mapping monotonicity. Hardware: 16-track pattern, mute/solo punching during playback — no clicks (scope the output at mute edges); meter page on/off toggles link traffic as expected (packet counters).

Stages (one commit each): 1) `TrackMix` + engine application + host tests; 2) protocol 0x78/0x79 + tests + docs; 3) mixer page + solo logic; 4) meter subscribe path + bench click/soak test. (Stage B routing lands inside Phase 3's existing items; send FX inside Phase 5's.)
