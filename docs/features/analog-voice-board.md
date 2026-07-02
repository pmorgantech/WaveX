# Analog Voice Board — PCM1690 TDM, VCF/VCA, CV Calibration

**Status**: Target design with prototype fragments in code (`cv_bus.hpp`, `AudioOutputMode::VoiceSAI2`, PCM1690 config in `hardware_config.h`). Phase 3 in `roadmap.md`.
**Consolidates**: the Phase-II material from `archive/daisy_devel.md`, minus its contradictions.
**Staging**: the analog section ships in two stages selected by build flags (`architecture.md` §5.2/§5.3). This doc's Stage A is buildable today; Stage B is the voice board proper.

## 0. Stage A — Paraphonic Prototype (stereo codec + MCP4728)

Limited-scope analog testing before the voice board exists: all voices sum digitally to the **SAI1 stereo codec output**, which feeds **one shared analog VCF/VCA pair**; a single **MCP4728** provides the CVs.

```
Daisy SAI1 stereo out → shared VCF (SSI2144) → shared VCA (SSI2164) → out
MCP4728 (I2C, 400 kHz):  CH A = cutoff · CH B = resonance · CH C = VCA · CH D = spare
```

- **Build config**: `WAVEX_VOICE_OUTPUT_BACKEND = STEREO_MIX`, `WAVEX_CV_BACKEND = MCP4728`, `WAVEX_ANALOG_CV_GROUPS = 1`.
- **Paraphonic control law**: the shared filter/amp envelope **retriggers on every note-on** and enters release when the **last** active voice releases. Cutoff modulation source = the paraphonic envelope + global LFO; per-voice envelopes still shape each voice digitally (level only) before the sum, which keeps the mix articulate even with one analog VCA.
- **Timing budget**: one MCP4728 fast-write ≈ 225 µs @400 kHz — fits the 1 kHz tick comfortably for a single group. Staged in the control tick, flushed from the main loop (never blocking I2C in the callback — `architecture.md` §7).
- **What Stage A validates**: CV calibration workflow (gain/offset/curvature per `CvCal`), SSI2164 inversion, exponential cutoff mapping, control-tick → CV latency, and the analog signal levels — i.e. everything in §3 below, on one voice group, before committing to the PCB.
- **What it cannot validate**: TDM slot ordering, per-voice CV fan-out timing, inter-channel crosstalk on the voice board.

### Stage A → Stage B transition checklist

1. Flip flags: `VOICE_OUTPUT_BACKEND = TDM8`, `CV_BACKEND = MCP48`, `ANALOG_CV_GROUPS = 8` — nothing above the CV group router changes (`architecture.md` §5.3).
2. PCM1690 bring-up (§2) and slot-order verification.
3. Re-run the §3 calibration per voice; the Stage-A calibration procedure and UI page are reused, table already sized for 8 groups.
4. Keep the Stage A configuration buildable in CI (a cheap compile matrix over both flag sets) — it remains the fallback/bring-up config for new boards.

## 1. Signal chain (Stage B target)

```
Daisy SAI2 (TDM-8 master TX, 48 kHz, 8×32-bit slots, 24-bit data)
   → PCM1690 8-ch DAC (I2C-configured, differential outs)
   → per-channel 2nd-order LPF + diff→SE stage
   → per-voice VCF (SSI2144) → VCA (SSI2164)
   → analog summing bus → main outs
CV control: Daisy → CV DACs → V/oct-ish cutoff CV, resonance CV, VCA CV per voice
```

The Daisy's on-board codec (SAI1) remains: stereo **input** (sampling/recording) and the stereo **digital master mix** output (headphones/monitor path, also the Phase-1 fallback).

## 2. PCM1690 bring-up checklist (as designed)

1. Clocks: MCLK 12.288 MHz (256×Fs) out of SAI2; BCLK 12.288 MHz (48 k × 8 slots × 32 bits); LRCK/FS 48 kHz. Frame = 256 bits. Pins per `pin_config.h` (`WAVEX_DAISY_PCM_*`, D21–D24).
2. Reset sequencing: hold RST low through power-up → release → I2C init (MODE pin low = software control). TDM data on DIN1 only.
3. Register init: 24-bit TDM format, de-emphasis off, per-channel attenuation 0 dB, unmute last. Config constants already exist in `hardware_config.h` (`WAVEX_PCM1690_*`) — drive the driver from those, don't hardcode.
4. Verification: send 8 distinct test tones, confirm slot→jack order with a scope before wiring the analog board. Series 22–33 Ω on clock lines if ringing.
5. DMA: SAI2 circular DMA with ping-pong buffers of interleaved `int32` frames; `float/q15 → 24-bit-left-justified-in-32` pack (`float_to_int24` design exists). Buffers obey `architecture.md` §7.1 (alignment + placement).

## 3. CV subsystem

### Hardware (Stage B part selection open — roadmap Phase 3.1)

Stage A uses the **MCP4728** (I2C, 4×12-bit, addr 0x60, 400 kHz) already in `cv_bus.hpp`. Stage B pin plan: **4× MCP48CMB28** (SPI, dual 12-bit) on shared SPI with per-chip CS (D25–D30).

Budget check at the 1 kHz control tick, 8 voices × 3 CVs = 24 channels/tick:
- I2C @400 kHz: ~25 µs/byte → a 4-ch fast write ≈ 225 µs; 6 devices ≈ 1.35 ms → **does not fit the tick**. Multiple I2C buses or 1 MHz FM+ would still be marginal.
- SPI @12 MHz: 24-bit frame ≈ 2 µs + CS overhead; 24 channels ≪ 100 µs → comfortable, and DMA-able.

**Recommendation**: SPI DACs for the Stage B voice board; the MCP4728 is the sanctioned Stage A backend, not a scaling path.

### Control mapping (validated design from prototyping, keep)

- **VCA (SSI2164)**: control is *inverted* — 0 V ≈ 0 dB (loud), +3.3 V ≈ −100 dB. Invert the envelope before the DAC (`vca_inv = 1 − vca`).
- **VCF (SSI2144)**: exponential cutoff feel via `y = (e^{kx} − 1)/(e^k − 1)`, k ≈ 3, tuned by ear.
- **Per-voice calibration** (`CvCal`): gain/offset per CV + curvature k, calibrated per board: measure corner frequency at cutoff = 0 and 1, fit linear map to the target voltage range; verify VCA "silent is truly silent". Calibration table persists on SD; a UI calibration page walks the procedure.

### Timing rules

- CV values are **staged** in the control tick (audio context) and **flushed** from the main loop via DMA/IT transfers — no blocking bus transactions in the callback (`architecture.md` §7).
- CV update completes within the same 1 ms tick it was staged in (scope-verified gate in roadmap Phase 3).
- Gate outputs are direct GPIO writes (nanoseconds) and may be set in the tick.

## 4. Voice-count contract

Everything above assumes **8 voices × (cutoff, resonance, VCA) + spare CV headroom**. If the voice count or CV-per-voice changes, update in this order: this doc → DAC count/pinout in `pin_config.h` → `CvBus` mapping → TDM slot map. The TDM slot index, voice manager index, and CV channel group for a voice must always be the same number — one voice, one index, everywhere.
