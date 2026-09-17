# WaveX Component Decisions

## Backend Processor

### NXP i.MX RT1176

Module: SOMLabs VisionSOM-RT117x
Status: Selected

Purpose:

- main audio engine
- sample playback
- DSP
- audio/TDM
- control generation

References:

- datasheets/processors/VisionSOM-RT117x.pdf
- [RT1170 consumer B-silicon datasheet](../datasheets/IMXRT1170BCEC.pdf) (assumed; confirm fitted silicon on arrival)
- datasheets/processors/IMXRT1170RM.pdf

Do not assign processor pins without checking the SOM pinout.

---

## Analog Filter

### SSI2140

Status: Planned

Supply:

- TODO verify

Purpose:

- per-voice analog VCF

Reference:

- datasheets/analog/SSI2140.pdf

---

## VCA / Pan

### SSI2190

Status: Planned

Purpose:

- per-voice amplitude and stereo pan

Reference:

- datasheets/analog/SSI2190.pdf
