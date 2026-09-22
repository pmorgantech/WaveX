# Virtual-analog filters: lessons for WaveX

**Status:** Research and proposed comparisons, 2026-09-22. No DSP change or new
topology is approved by this note. Implementation order remains in the
[roadmap](../roadmap.md).

WaveX already uses the central idea in the supplied virtual-analog overview:
topology-preserving filters. The useful next step is to characterize their
modulation and nonlinear approximations, not replace them based on product-era
comparisons. This note condenses the applicable lessons for firmware and future
analog-board work, separating current behavior from experiments.

## Contents

- [What WaveX already has](#what-wavex-already-has)
- [Lessons worth keeping](#lessons-worth-keeping)
- [The ladder comparison that matters](#the-ladder-comparison-that-matters)
- [Antialiasing without breaking the filter](#antialiasing-without-breaking-the-filter)
- [Analog hardware implications](#analog-hardware-implications)
- [Proposed evaluation order](#proposed-evaluation-order)
- [Related and primary references](#related-and-primary-references)

## What WaveX already has

| Area | As-built behavior and implication |
|---|---|
| [VoiceFilter](../../firmware/daisy/src/audio/voice_filter.hpp) | Instrument-owned topology, mode, slope and drive; runtime state belongs to each allocated voice renderer. SVF and Ladder share cutoff/resonance controls, with topology-specific response laws. |
| [SVF](../../firmware/daisy/src/audio/svf_filter.hpp) | Trapezoidal/TPT state-variable filter, LP/BP/HP/Notch. The 24 dB option cascades a resonant stage with a fixed-Q second stage. Input-only cubic saturation leaves the feedback equations linear. Q is limited to 16: ringing is intentional, sustained self-oscillation is not. |
| [Ladder](../../firmware/daisy/src/audio/ladder_zdf.hpp) | Four TPT one-poles, algebraically solved **linear** global feedback, then one rational soft clipper. No oversampling or nonlinear iteration. LP/BP/HP are stage-tap mixes; the wrapper's Notch is input minus BP12, irrespective of slope. |
| [Tuning](../../firmware/daisy/src/audio/fast_tan.hpp) | Prewarping uses a polynomial below 12 kHz and `tan` above it at 48 kHz. The ladder clamps internal cutoff to 5 Hz–0.45 × sample rate; wrapper cutoff boundaries have explicit bypass/silence semantics. “Exact to Nyquist” is not our implementation contract. |
| [Modulation](param-locks-and-modulation.md#3-modulation-matrix-instrument-scoped) | Cutoff/resonance destinations update once per nominal 1 ms control block. Per-sample filter processing does **not** mean audio-rate parameter modulation. |

Keep Instrument parameters, derived coefficients and integrator history distinct.
The callback owns evolving filter state; control handoffs carry complete values.
New solver/antialiasing history must follow the same voice lifetime, stereo
channel isolation, initialization and reset rules. DSP must remain independent
of CV DACs and output sinks (principles 3–6).

The [2026-09-14 comparison](../callback-performance-log.md#filter-topology-ab--2026-09-14)
measured callback peaks of **30.25% SVF, 33.65% ZDF, 47.48% 2× Huovilainen,
69.82% 4× Huovilainen**. Those were 200-second, eight-voice comparisons without
sequencing, streaming or locks, and included cutoff bypass periods. They justify
the earlier removal of those oversampled ladders; they do not establish today's
headroom or universal sonic equivalence.

## Lessons worth keeping

- **Topology and state matter when controls move.** For fixed parameters,
  equivalent TPT and direct-form realizations can implement the same bilinear
  transfer function. Their changing-parameter behavior need not match.
- **Feedback memory is necessary; an arbitrary extra loop delay is not.**
  Using a previous output alone does not diagnose a bad filter. The naive
  Euler coefficient choice and loop discretization must be examined together.
- **ZDF is not antialiasing or a universal stability proof.** Prewarping fixes
  a chosen frequency for a frozen linear model; nonlinear harmonics and
  modulation sidebands can still alias. The Nyquist endpoint is singular.
- **Solve the actual nonlinearity.** Saturating a linear solution is an
  approximation. One or two Newton steps are a cost choice, not a convergence
  guarantee. These distinctions follow the treatment in
  [Zavalishin, chapters 3–6](https://www.discodsp.net/VAFilterDesign_2.1.2.pdf).

The supplied one-pole exercise cannot demonstrate resonance by itself. Use it
for cutoff/transient comparisons; use a resonant SVF or ladder for resonance
and self-oscillation. Do not retain the unsourced Ion/Nord/Virus implementation
guesses, universal product-era sound claims, or broad conclusions about neural
filters from unspecified experiments.

## The ladder comparison that matters

**Derived from our implementation:** let `G` be one stage's instantaneous gain,
`S` the four stored states' contribution to the final output, `k` the feedback
gain, `x` the already drive/level-compensated input, and `phi` the current clipper.
With the states frozen for one sample:

```text
y4 = G^4 * u + S
current approximation: u = phi((x - k*S) / (1 + k*G^4))
nonlinear-loop model:  u = phi(x - k*(G^4*u + S))
```

These agree when `phi` is the identity, but generally differ under saturation.
The second equation is a useful **reference candidate**, not a claim to model
every transistor or the SSI2144. Its difference from current output should be
measured before adding complexity.

First compare a converged host reference using the **same** clipper and gain
laws. Measure residual error, resonance pitch/onset, decay, level and spectrum
over cutoff, drive and input level. A later device candidate needs a fixed
iteration cap, defined failure behavior and DWT measurements. Iterations must
not advance integrator history: commit state once after selecting the sample's
solution. This preserves deterministic work and single ownership (principles
1, 2, 5 and 10).

Per-stage saturation is a separate sound/model change. The current SVF input
drive deliberately avoids damping its resonant state; inserting a limiter
there would change resonance and gain. Compare these choices independently.

## Antialiasing without breaking the filter

The first contained experiment is **antiderivative antialiasing (ADAA) on the
SVF input clipper**, outside its feedback loop. For a fixed waveshaper `phi`
and antiderivative `F`, the first-order expression is:

```text
(F(x[n]) - F(x[n-1])) / (x[n] - x[n-1])
```

It needs a numerically safe limit for nearly equal inputs and a continuous
piecewise antiderivative at the clamp boundaries. The low-level response
introduces half a sample of delay and high-frequency attenuation; account for
the dry/drive blend, time-varying drive and exact drive-zero behavior. Inserting
ADAA inside the ladder loop requires a new loop derivation, not a replacement
of `Clip()`. See [Parker, Zavalishin and Le Bivic, DAFx 2016,
§§4–6](https://www.dafx.de/paper-archive/2016/dafxpapers/20-DAFx-16_paper_41-PN.pdf).

Oversampling remains a comparison option. Include interpolation, decimation,
state, delay and control-rate mapping in its cost. In a nonlinear feedback
filter, the entire coupled loop may need the higher rate even though some
elements are linear. An output lowpass cannot remove aliases already folded
into the wanted band. LP, HP and BP outputs need separate spectral checks.

Prefer libDaisy/DaisySP/CMSIS-DSP support where equivalent; a generic biquad
kernel is not automatically a replacement for changing TPT state or a nonlinear
solver. Preserve the existing q15 sample path and float filter state unless
measurement justifies a conversion (principles 9–10).

## Analog hardware implications

ZDF addresses digital discretization; it does not upgrade a physical VCF.
If the [analog board](analog-voice-board.md) is revived, measure cutoff/CV and
resonance laws, self-oscillation tracking, input headroom, distortion versus
level, output gain and CV settling/feedthrough on the actual circuit. Those
measurements can guide controls and a later digital reference. Do not infer
audio-rate analog FM from a 1 kHz staged CV path or software smoothing.

The Stage A output/CV configuration remains buildable; that is not evidence
that its external analog path has passed listening/calibration tests. Phase 3
and physical analog hardware remain deferred. A shared post-mix VCF also
cannot reproduce independent per-note filtering in a chord.

WDF is worth considering for a specific measured circuit, with stability claims
limited to the applicable passive elements/interconnections; active resonant
circuits and nonlinear solvers require further analysis. See
[Smith's WDF treatment](https://ccrma.stanford.edu/~jos/pasp/Wave_Digital_Filters_I.html).
The nodal/DK approach is a more general schematic-derived alternative, described
by [Yeh, Abel and Smith](https://ccrma.stanford.edu/~dtyeh/papers/yeh10_taslp.pdf).
Neither is a reason to add a general circuit solver to this callback. Neural
models, oscillator BLEP/DPW work and additional filter families remain separate,
unscheduled research.

## Proposed evaluation order

1. **Characterize the current filters.** Extend the existing
   [SVF](../../firmware/daisy/tests/unit/audio/svf_filter_test.cpp) and
   [VoiceFilter](../../firmware/daisy/tests/unit/audio/voice_filter_test.cpp)
   coverage with response/spectral measurements and level-matched listening.
   Include high drive/resonance, HP/BP, the 12 kHz tuning crossover, cutoff
   boundaries, topology/mode/slope changes and held-note edits. Existing finite
   output tests do not establish click-free or alias-free sound.
2. **Separate control artifacts from nonlinear aliases.** Compare current
   block-held cutoff/resonance with a proposed bounded parameter ramp. Define
   the interpolation domain and coefficient derivation before implementing it;
   smoothing is not audio-rate modulation and must not blur step-lock intent.
3. **Compare one change at a time.** Try SVF-input ADAA and the ladder reference
   above independently. Use a convergence-checked high-rate reference with
   proper resampling to distinguish intended harmonics from alias energy.
   Test zero-state silence separately from impulse-seeded self-oscillation.
4. **Require device acceptance before adoption.** Record matched QSPI-image
   identities, DWT peak/average, memory and full-workload results, then update
   [hardware validation](../hardware-validation.md) with listening, transitions
   and soak procedures in the implementation change. No new hardware result is
   claimed here.

This is Phase 2 filter validation plus an **unscheduled quality backlog**.
The current **85.3713% and 86.5040%** callback findings, mixed-channel one-hour
soak, SD recovery and physical panel/MIDI gates remain open. The previous
sampling/arpeggiator continuation exception does not authorize these additional
callback costs. Follow the [capacity checkpoint](../roadmap.md#2c--callback-capacity-checkpoint)
and [headroom gate](../performance_monitoring.md#callback-headroom-gate).

## Related and primary references

- [Project principles](../project-principles.md), [architecture](../architecture.md)
  and [Daisy guide](../daisy_rt_audio_coding_guide.md): ownership and real-time rules.
- [Andrew Simper, trapezoidal SVF derivation](https://cytomic.com/files/dsp/SvfLinearTrapOptimised2.pdf):
  the linear state-variable equations underlying the existing implementation.
- [Zavalishin, The Art of VA Filter Design, revision 2.1.2](https://www.discodsp.net/VAFilterDesign_2.1.2.pdf):
  chapters 3–6; the author's book hosted on its distribution mirror.
- [DAFx 2016 antialiasing companion code](https://github.com/julian-parker/DAFX-AntiAliasing):
  numerical comparison material, not a drop-in embedded implementation.
