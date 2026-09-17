# Vintage sampler math at a fixed 48 kHz engine rate

**Status: research proposal, 2026-09-17; no firmware implementation.** This note
develops a companded sampler model for WaveX. The engine and codec remain at
48,000 Hz; each voice simulates a variable playback clock. Offline preparation
belongs with Phase 4; new playback reconstruction needs a separately scoped,
measured runtime gate in the [roadmap](../roadmap.md).

## Contents

- [Data and ownership](#data-and-ownership)
- [Capture clock and playback clock](#capture-clock-and-playback-clock)
- [Companding with an exact zero](#companding-with-an-exact-zero)
- [Capture filtering](#capture-filtering)
- [Reconstructing a moving DAC clock](#reconstructing-a-moving-dac-clock)
- [Musical filter and saturation](#musical-filter-and-saturation)
- [Runtime cost and ARM library choices](#runtime-cost-and-arm-library-choices)
- [Numerical evidence and remaining work](#numerical-evidence-and-remaining-work)
- [Historical confidence and related references](#historical-confidence-and-related-references)

## Data and ownership

Keep the existing Sample Pool / Instrument / Zone ownership described in the
[instrument model](instrument-model.md). These are conceptual fields to resolve
within that model, not a new file format or voice hierarchy:

| Entity | Authoritative data | Lifetime / constraint |
|---|---|---|
| Preparation recipe | Source identity, capture rate, input trim, capture filter, compander/quantizer version | Saved with a newly rendered asset; original remains unchanged |
| Prepared asset | Decoded PCM16, actual capture rate, root/tuning, remapped sample/loop markers | Immutable during playback; decoded PCM saves callback work but gives no 8-bit storage saving |
| Playback settings | Reconstruction choice, post-filter, optional saturation | Saved sound settings; complete schema remains to be designed |
| Sounding voice | Fractional source position, held value, filter state, current rate ratio | Private runtime state; stereo channels share timing and retain separate signal state |
| Engine | 48,000 output frames/second | Fixed independently of all voice clocks |

Do not resample the prepared asset back to 48 kHz and then lose its capture-rate
identity. A complete offline print remains useful for a fixed-pitch one-shot,
but later transposition moves its baked filter/saturation response too. The
requested live mode must regenerate playback artifacts at each played pitch.

## Capture clock and playback clock

Let `Fc` be the asset's capture rate, `Fe = 48000`, and `s` the pitch interval in
semitones, including fine tuning in hundredths of a semitone. Apply the existing
note/root/tuning calculation exactly once:

```text
r = 2^(s / 12)
Fv = Fc * r                       virtual playback clock, Hz
increment = Fv / Fe               source frames per output frame
u[n + 1] = u[n] + increment       fractional source position
```

For a 27,700 Hz asset:

| Pitch | Virtual clock | Source increment at 48 kHz | Duration relative to root |
|---|---:|---:|---:|
| −12 semitones | 13,850 Hz | 0.288541667 | 2× |
| Root | 27,700 Hz | 0.577083333 | 1× |
| +12 semitones | 55,400 Hz | 1.154166667 | 0.5× |

The +12 case is valid even though the virtual clock exceeds the output rate.
It can contain two DAC transitions between output frames. Filtering a held
signal requires accounting for both; merely selecting the last source value
misses their contribution. Preserve fractional position across blocks. Define
note-on clock phase, loop boundaries, release and pitch-modulation interpolation
before implementing them; the numerical experiment below covers constant pitch.

For time-varying pitch, source position is the integral of `Fc * r(t)`;
transitions occur when that position crosses an integer. Glide changes this
trajectory continuously. It must not reset the clock or recapture the sample.
Arbitrary-time evaluation and interpolation are covered by
[Smith's resampling reference](https://ccrma.stanford.edu/~jos/resample/).

## Companding with an exact zero

The smooth compressor is useful as a creative model, with `mu = 255`:

```text
C(x) = sign(x) * log1p(mu * abs(x)) / log1p(mu)
E(y) = sign(y) * expm1(abs(y) * log1p(mu)) / mu
```

Clamp normalized input to `[-1, 1]` after input trim. The supplied
`round((y + 1) * 127.5)` endpoint mapping has no zero reconstruction level:
zero input expands to `+0.0000862116`, approximately −81.29 dBFS DC.

A simple alternative uses a sign and seven magnitude bits:

```text
j = min(127, floor(127 * abs(C(x)) + 0.5))
yq = sign(x) * j / 127
decoded = E(yq)
```

This has **255 distinct levels**, with two possible sign codes for zero. It is
monotone, symmetric and preserves silence exactly. It deliberately trades one
distinct level for these properties. This is our proposed generic quantizer,
not a normative G.711 encoder or a verified Emulator II converter.

The expander slope explains the character:

```text
dE/dy = log(1 + mu) / mu * (1 + mu)^abs(y)
local decoded step ≈ (dE/dy) / 127
```

Small signals receive fine steps; peaks receive coarse steps. Input trim
therefore changes the quantization character. Automatic normalization would
hide that behavior. Keep dither off in the first deterministic reference;
adding noise before the compressor and adding it in the compressed domain
produce different noise distributions after expansion.

A hardware-calibrated alternative should use a measured decoder table `D[c]`
and encoder decision thresholds `T[c]`. Do not assume that nearest midpoints
between DAC levels reproduce a particular successive-approximation converter.
The [ITU G.711 specification](https://www.itu.int/rec/T-REC-G.711/en) defines a
separate standardized codec; the smooth formula does not establish byte-code
compatibility.

## Capture filtering

Model capture as a causal input filter followed by evaluation at
`t[k] = k / Fc`, then companded quantization. Fractional capture times should
not be snapped to the nearest 48 kHz input frame. Offline interpolation can
estimate the input waveform at those times, using a sufficiently accurate
reference before choosing a bounded implementation.

Distinguish a clean resampling mode from a character mode. A converter that
automatically removes everything above `Fc/2` prevents the intentional input
foldover a weak vintage-style capture filter might allow. A character reference
can reconstruct the original input band, apply the chosen causal filter and
then evaluate at the capture clock. Residual content above capture Nyquist
folds at this last sampling step.

For an analog Butterworth low-pass of order `N`:

```text
|H(f)|² = 1 / (1 + (f / cutoff)^(2N))
```

At `cutoff = 0.45 * 27700 = 12465 Hz`, attenuation at capture Nyquist is only
**5.215 dB for four poles** or **8.059 dB for eight poles**. These are analytical
analog responses; a discrete filter has its own frequency mapping. Thus this
cutoff is a sound-design starting point, not a strong anti-alias guarantee.
Specify a stopband requirement separately for clean conversion.

Forward/backward filtering such as `sosfiltfilt` squares magnitude and removes
phase; it is unsuitable as a reference for one causal analog pass. Any digital
filter cutoff must also be below its actual processing Nyquist. Do not use a
fixed 12.5 kHz cutoff on a 22.05 kHz input.

## Reconstructing a moving DAC clock

For constant pitch, a zero-order-hold DAC model is:

```text
d(t) = decoded[floor(Fv * t)]
```

With normalized `sinc(x) = sin(pi*x)/(pi*x)`, its normalized hold response is:

```text
Hhold(f) = exp(-j*pi*f/Fv) * sinc(f/Fv)
```

It creates images around multiples of `Fv`, with sinc-shaped rolloff. Both
image positions and rolloff change with pitch. These are core reasons why
clean interpolation alone does not sound like a held DAC. See
[Analog Devices' DAC reconstruction explanation](https://www.analog.com/en/resources/technical-articles/equalizing-techniques-flatten-dac-frequency-response.html).

The host-rate shortcut `decoded[floor(u[n])]` samples the discontinuous hold
without first reconstructing/filtering it. This can fold images above 24 kHz
into the audible band. It is a useful crude effect, but the extra foldover is
not evidence of historical accuracy.

An exact timing reference is possible for linear continuous-time filters.
For one pole, between two DAC events the input `h` is constant:

```text
z(t + dt) = h + (z(t) - h) * exp(-2*pi*cutoff*dt)
```

Advance the filter to every source transition, change `h`, then advance to the
next 48 kHz observation time. This handles fractional events and clocks above
48 kHz without rounding transitions to host frames. The included experiment
uses this equation as an independent timing reference, **not as the proposed
SSM-style filter or a complete anti-alias solution**. One pole leaves substantial
out-of-band energy before the final 48 kHz sampling step.

For a higher-order linear filter `dz/dt = A*z + B*h`, the corresponding interval
update is `exp(A*dt)*z + integral(exp(A*tau), tau=0..dt)*B*h`. It can support an
offline reference; repeated matrix exponentials are not a callback proposal.

Implementation candidates to compare against that reference are:

- Oversampled reconstruction with fractional event handling, musical filter
  and optional saturation, followed by an output decimator below 24 kHz.
- A bandlimited step reconstruction at 48 kHz, followed by a separately
  evaluated filter/nonlinearity path.

Simply oversampling and snapping events to the new grid still introduces timing
error; measure convergence and spurious tones. Select oversampling factor,
kernel length and supported pitch range from spectral error and device budget.
Bound the maximum events per frame explicitly. Use existing CMSIS-DSP kernels
where they cover the selected algorithm, with sequential/batched sample access.

Keep three phenomena separate: intentional capture aliasing, audible DAC
images, and accidental aliasing introduced by the numerical model's output
rate. A mode may deliberately include the last, but it should say so.

## Musical filter and saturation

Start with a clearly named 12 or 24 dB/oct low-pass approximation, roughly
8–12 kHz, with modest resonance. Cutoff stays in absolute Hz unless explicit
keyboard tracking changes it. A reproducible digital baseline is the
[W3C/RBJ biquad low-pass](https://www.w3.org/TR/audio-eq-cookbook/), evaluated at
the actual processing rate `Fp`:

```text
w = 2*pi*cutoff/Fp; a = sin(w)/(2*Q)
b = [(1-cos(w))/2, 1-cos(w), (1-cos(w))/2]
a_coeff = [1+a, -2*cos(w), 1-a]
normalize every coefficient by a_coeff[0]
```

For a nonresonant fourth-order Butterworth baseline, cascade sections with
`Q = 0.5411961` and `1.3065630`. An arbitrary resonance adjustment changes that
response. These coefficients do not model an SSM2045 circuit.

Gentle saturation follows the musical filter. A simple memoryless baseline,
with unity small-signal slope and separate makeup gain, is:

```text
f_g(x) = tanh(g*x) / g                  g > 0; bypass at g = 0
A_g(x) = log(cosh(g*x)) / g²             antiderivative
```

First-order antiderivative antialiasing (ADAA) gives:

```text
y[n] = (A_g(x[n]) - A_g(x[n-1])) / (x[n] - x[n-1])
```

When the denominator is very small, use `f_g((x[n]+x[n-1])/2)` to avoid
cancellation. Evaluate `log(cosh(z))` stably: near zero use
`log1p(2*sinh(z/2)^2)`; for large magnitude use
`abs(z) + log1p(exp(-2*abs(z))) - log(2)`.

ADAA introduces its own delay and frequency response; it is not a transparent
drop-in. Compare it with oversampling before choosing either. The method comes
from [Parker, Zavalishin and Le Bivic, DAFx 2016](https://dafx16.vut.cz/dafxpapers/20-DAFx-16_paper_41-PN.pdf).
This tanh model has no transformer memory or tape hysteresis; those names would
overstate what it simulates.

## Runtime cost and ARM library choices

**Cost analysis, not a measured implementation.** At 480 MHz / 48 kHz there
are 10,000 CPU cycles per output frame for the entire engine. A 48-frame callback
has a 480,000-cycle deadline. Apply the existing
[worst-case headroom gate](../performance_monitoring.md#callback-headroom-gate)
to the whole callback; these are total budgets, not spare capacity.

The user confirmed the intended near-term **RT1176 migration** on 2026-09-17.
At a validated 1 GHz M7 clock, the same 48 kHz / 48-frame engine has about
20,833 cycles per frame and 1,000,000 cycles per callback: 2.083 times the raw
cycle budget of the 480 MHz H750. At 800 MHz those figures are 16,667 and
800,000, or 1.667 times. Use the selected part's allowed operating point and
the configured clock, not the family's maximum specification. This is clock
arithmetic, not a measured DSP speedup; memory placement and the board port
still determine actual timings.

NXP's [RT1170 data sheet](https://www.nxp.com/docs/en/data-sheet/IMXRT1170BCEC.pdf)
specifies an M7 up to 1 GHz, 32 KB instruction/data caches, up to 512 KB M7 TCM,
and a separate M4 up to 400 MHz. These help with hot kernels, lookup tables and
scratch buffers. Follow the [migration plan](../rt1170-migration.md): budget the
audio path on the M7 alone initially. The M4 does not automatically add cycles
to a serial callback. The CMSIS M7 kernels remain applicable; this upgrade does
not add NEON or Helium. Evaluate 2×/4× reconstruction on the new board, without
promising a polyphony count before measuring it.

WaveX already has an integer-frame plus Q24 fractional playback cursor and
native-rate compensation in `VoiceSampleState` / `PlaybackPhase` in
[`voice_manager.hpp`](../../firmware/daisy/src/audio/voice_manager.hpp).
Its RAM renderer uses PCM16 source data with float interpolation and filtering.
Reusing that cursor does not require evaluating `pow`, `log` or `exp` per output
sample. The extra feature cost is primarily the chosen reconstruction and
nonlinearity, plus their state and memory traffic.

| Stage | Candidate implementation | Cost character |
|---|---|---|
| Capture filtering and companded quantization | Offline render to decoded PCM16 | No added compander work during playback |
| Optional packed-code decode | 256-entry PCM16 lookup table (512 bytes) | One lookup per source code; a storage alternative, not required for this feature |
| Source speed | Existing fractional cursor; rate calculated at note/control updates | Small fixed arithmetic per frame |
| 12/24 dB musical low-pass | One/two float biquads | 5/10 multiplications and 4/8 additions per channel per processing sample, excluding loads/control |
| Held DAC with corrected fractional edges | Bounded step kernel or oversampled reconstruction | Depends on pitch, kernel length and quality target; not yet selected |
| Post-filter saturation | Bounded lookup/polynomial approximation; optional oversampling | More predictable than repeated transcendental functions; alias/error testing still required |
| Exact continuous-time research model | Exponentials at fractional events | Keep as an offline reference until a bounded approximation is measured |

For scale, two biquads at 48 kHz require 0.48 million multiplications/second per
mono voice, or 7.68 million across 16 mono voices. Running those filters at 4×
costs 30.72 million multiplications/second across 16 voices. An illustrative
32-tap decimator producing 48 kHz adds 24.576 million multiply-accumulates/second
across those voices, before reconstruction, saturation or ordinary playback.
Stereo doubles these channel operations. These are operation counts, not CPU
percentages or a claim that 32 taps meet the required stopband specification.

To translate a future measurement, an added `C` cycles per mono voice per output
frame across `V` voices costs approximately `100 * V * C / 10000` percent of
the 480 MHz CPU. For example, a **hypothetical measured** 50 cycles across 16
voices would add 8 percentage points of CPU use. Allocation, note starts,
control updates, memory stalls and the existing renderer remain additional
costs; only whole-callback maximum DWT timing can pass the gate.

Use the already vendored CMSIS-DSP implementation where it fits:

- [`arm_biquad_cascade_df2T_f32`](https://arm-software.github.io/CMSIS-DSP/latest/group__BiquadCascadeDF2T.html)
  and its stereo variant provide block filters. Process preallocated blocks
  rather than making one external library call per voice sample. Benchmark
  block staging against the existing fused renderer; buffering has a cost.
  Preserve CMSIS's feedback-coefficient sign convention.
- [`arm_fir_decimate_f32` / `arm_fir_decimate_q15`](https://arm-software.github.io/CMSIS-DSP/latest/group__FIR__decimate.html)
  compute only retained outputs. Use them for fixed-factor final decimation
  when oversampling is selected. The `fast_q15` variant uses a narrower
  accumulator and needs proven headroom; overflow is not an acceptable way to
  generate character.
- [`arm_fir_interpolate_f32` / `arm_fir_interpolate_q15`](https://arm-software.github.io/CMSIS-DSP/latest/group__FIR__Interpolate.html)
  provide integer-factor polyphase interpolation. They are useful building
  blocks, not a ready-made arbitrary-rate, pitch-modulated held-DAC model.
  Filtering out all images would also change the intended reconstruction.
- The vendored Q15 FIR has packed ARM DSP multiply-accumulate paths. Retain
  PCM16 for sample storage and benchmark Q15 for suitable FIR work; use float
  where the existing voice/filter path already does. Repeated conversions or
  changing the whole renderer's representation can erase a kernel-level win.

The Cortex-M7 has DSP instructions and a single-precision FPU, but no NEON or
Helium vector unit. In particular, CMSIS's
[`arm_vexp_f32` scalar path](https://github.com/ARM-software/CMSIS-DSP/blob/main/Source/FastMathFunctions/arm_vexp_f32.c)
calls `expf` in a loop, and the corresponding `arm_vlog_f32` uses `logf` on this
target. Merely using the fast-math namespace does not accelerate those calls.
Avoid repeated `log1p`/`expm1`/`tanh`/`log(cosh)` in the sample loop by doing
preparation offline or using validated tables/approximations.

Prefer the current renderer and its existing filters before adding duplicate
stages. CMSIS does not supply the complete proposed vintage sampler. A small
bounded phase/event adapter will still be needed around the selected kernels.
Do not change compiler floating-point semantics globally for this experiment;
the current build and baseline timings must remain comparable. Keep hot state
in fast internal memory and batch source reads. No library choice compensates
for poor SDRAM access or an unbounded high-pitch event loop.

## Numerical evidence and remaining work

Prerequisite: Python 3 in the WaveX devcontainer; no extra packages. Run:

```bash
python3 tools/vintage_sampler_math.py
```

The [reference experiment](../../tools/vintage_sampler_math.py) passed on
2026-09-17. It checks monotonicity, symmetry, exact silence, compander inverse,
analytical one-pole step responses, event counts, block-size invariance, and
the saturation antiderivative. It prints deterministic JSON.

Quantization-only sine tests use 8,192 samples containing 101 cycles. The linear
comparison uses the same sign/magnitude quantizer with the compander bypassed.
Values are signal/error RMS ratios; quantization distortion is included in the
error. They are not measured converter specifications:

| Sine peak | Companded signal/error ratio | Linear signal/error ratio |
|---|---:|---:|
| 0 dBFS | 38.400 dB | 49.949 dB |
| −20 dBFS | 38.143 dB | 29.494 dB |
| −40 dBFS | 34.853 dB | 11.113 dB |
| −60 dBFS | 22.648 dB | 0.000 dB (output is zero) |

A synthetic 1 kHz captured tone, through the held-DAC/one-pole timing reference,
measured 500.010, 1000.006 and 2000.006 Hz at −12, 0 and +12 semitones. Output
remained 48 kHz throughout. Rendering in one block or six unequal blocks gave
identical arrays; the +12 case correctly processed two transitions in some
output intervals. Frequency estimates use interpolated positive zero crossings.

These checks establish selected arithmetic and timing properties. They do not
test audio files, spectral rejection, dynamic pitch, loops, stereo, PCM16
rounding, hardware playback or subjective similarity. No device speed or
callback-headroom claim follows from the Python experiment.

Before production work, choose an error target and compare spectra for silence,
impulses, sweeps and near-Nyquist tones over the intended pitch range. Compare
candidate outputs with an oversampling-converged reference, then audition real
one-shots. Any runtime proposal needs DWT worst-case measurements, a fixed voice
budget and an on-device zero-underrun gate; add its bench procedure to
[hardware validation](../hardware-validation.md) when implementation reaches it.

## Historical confidence and related references

The primary [Emulator II service manual](https://archive.org/details/e-mu_Emulator_II_Service_Manual),
printed page 2-17, identifies a 6072 DAC, SSM2045 VCF/VCA and S3528
switched-capacitor filter, and describes using the DAC in the sampling converter.
That supports modeling separate conversion and filter stages. It does not
establish that the smooth mu=255 curve plus uniform quantization reproduces
the converter transfer function. A verified 6072 codebook, input thresholds,
filter response and clock measurements remain missing.

Treat 27,700 / 22,050 / 13,850 Hz as requested creative settings until a specific
machine's clocks are verified. Do not infer historical RAM-saving modes or a
complete EII emulation from those numbers.

- [Architecture notes: vintage sampler grit](../architecture-notes.md#vintage-sampler-grit)
- [Offline sample editing](offline-sample-editing.md)
- [Project principles](../project-principles.md)
- [Daisy real-time audio guide](../daisy_rt_audio_coding_guide.md)
