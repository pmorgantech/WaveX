# WaveX Analog Voice / Audio Board Electrical Specification

**Status:** architecture frozen enough for schematic partitioning; component application circuits and rail details still require bench/design verification before fabrication.

## 1. Scope

This board contains the complete mixed-signal audio path for the production eight-lane analog architecture:

- AD1938 4-ADC / 8-DAC codec,
- 4 × MCP48CVB28 octal 12-bit CV DACs,
- 8 × SSI2140 analog filters,
- 4 × SSI2190 six-input voltage-controlled mixers used to realize stereo gain/pan and summing,
- stereo sampling input conditioning,
- reconstruction / buffering between AD1938 DAC outputs and filters,
- final left/right summing and output stages,
- local clean analog/digital supplies.

The RT1176 carrier sends TDM audio and digital control to this board. Audio does not return to the frontend PCB.

## 2. Physical-lane model

The production analog engine has **eight mono physical lanes**.

Each lane contains:

```text
AD1938 DAC[n]
  -> reconstruction/buffer
  -> SSI2140 VCF[n]
  -> split to Left and Right SSI2190 controlled inputs
  -> analog L/R summing buses
```

A logical mono voice consumes one lane. A true-stereo logical voice consumes two linked lanes. The allocator must treat those linked lanes atomically for trigger, release and stealing.

## 3. AD1938 codec

Target: **Analog Devices AD1938**, 48-lead LQFP.

Vendor-confirmed characteristics relevant to WaveX:

- four ADC channels,
- eight DAC channels,
- 24-bit audio,
- TDM modes,
- SPI configuration,
- separate 3.3 V analog and digital supplies,
- differential ADC inputs,
- single-ended DAC outputs,
- up to 192 kHz; WaveX uses 48 kHz.

Reference: https://www.analog.com/media/en/technical-documentation/data-sheets/AD1938.pdf

### 3.1 Digital audio

WaveX target:

- 48 kHz sample rate,
- 8 × 32-bit TDM slots,
- 256 BCLKs/frame,
- BCLK 12.288 MHz,
- 24 significant bits per slot,
- RT1176 SAI1 master,
- AD1938 ADC/DAC serial ports slave.

The first two ADC channels are used for stereo sampling input. ADC3/4 remain available for future use/test points unless a concrete feature claims them.

### 3.2 Codec power / references

Follow ADI guidance:

- 100 nF ceramic at each AVDD/DVDD pin,
- at least 22 µF local bulk capacitance,
- prefer separate/filtered analog and digital 3.3 V rails,
- FILTR: 10 µF || 100 nF close to pin,
- CM: 47 µF || 100 nF close to pin,
- keep analog reference/CM loading within datasheet limits.

The analog 3.3 V rail must be low noise and should not be sourced from the VisionSOM's limited VOUT-3V3 rail.

### 3.3 DAC output conditioning

The AD1938 outputs are single-ended, so no differential-to-single-ended converter is required.

Still required:

- reconstruction / anti-imaging filtering per ADI recommendations,
- DC/common-mode management before the SSI2140 input,
- gain scaling so the filter sees the intended synth-level headroom,
- protection from startup/reset transients.

Exact op-amp/filter values are a schematic-design task and must be based on the AD1938 evaluation circuit and desired SSI2140 drive level, not guessed.

### 3.4 Sampling inputs

Use two differential AD1938 ADC inputs for main sampling L/R.

Input stage requirements:

- user-selectable connector format decided mechanically elsewhere,
- ESD/RFI protection at the jack,
- input attenuation/gain appropriate for line/instrument source target,
- anti-alias/input RC network per ADI reference design,
- bias/common-mode network compatible with AD1938 differential ADC input,
- clip/headroom validation with the intended nominal input level.

Mic preamp/phantom power are out of scope unless explicitly added later.

## 4. CV DAC subsystem

Use **4 × MCP48CVB28**, not the MTP `CMB` variant unless a later manufacturing requirement justifies it.

Each device provides eight 12-bit outputs; four devices provide 32 channels.

Per physical voice lane:

| CV index | Function |
|---:|---|
| 0 | SSI2140 cutoff/exponential frequency control |
| 1 | SSI2140 resonance/Q control |
| 2 | SSI2190 Left gain/pan control |
| 3 | SSI2190 Right gain/pan control |

Total: 8 lanes × 4 CV = 32 DAC outputs.

### 4.1 SPI / latch

MCP48CVB28 characteristics from Microchip:

- 24-bit SPI command boundaries,
- up to 50 MHz writes / 25 MHz reads,
- two latch inputs on octal device:
  - LAT0 updates DAC0/2/4/6,
  - LAT1 updates DAC1/3/5/7,
- latch inputs permit synchronized update across one or multiple devices.

All four devices share SCK/SDI/SDO and use individual CS lines. Share LAT0 across all devices and LAT1 across all devices so the backend can load a complete frame and apply outputs together.

Reference: https://ww1.microchip.com/downloads/en/DeviceDoc/MCP48CXBX4_8-Family-Data-Sheet-20006556B.pdf

### 4.2 DAC reference / range

Do not freeze the reference topology until the required SSI2140 and SSI2190 control transfer functions are converted to actual voltages/currents.

The MCP48CVB28 supports VDD, internal band-gap, or external VREF options, with configurable output gain. The schematic shall provide a clean reference plan and per-control scaling/buffering as needed.

No filter/VCA control input should be connected directly to a DAC output until its required voltage/current range and polarity have been verified from the application circuit.

## 5. SSI2140 filters

Use **8 × SSI2140**, one per physical lane.

Useful device facts:

- four configurable transconductance sections,
- exponential cutoff control,
- on-chip Q/resonance VCA with linear current control,
- ±4 V to ±16 V supply range,
- current datasheet characterizes typical application at ±12 V.

Reference: https://www.soundsemiconductor.com/downloads/ssi2140datasheet.pdf

### 5.1 Initial topology

For first hardware, implement a stable four-pole low-pass topology based directly on Sound Semiconductor's published design/application material. Do not attempt a highly configurable multimode patch matrix on Rev A unless the additional switching is explicitly required.

Provide test points per lane for:

- filter input,
- filter output,
- cutoff control node,
- Q control node.

### 5.2 Control conditioning

Cutoff and Q are not generic 0–3.3 V inputs. The DAC interface must translate the digital control range into the SSI2140's required exponential-control voltage and Q control current/range.

The final transfer functions belong in this document once the selected reference circuit is captured and simulated/measured.

## 6. SSI2190 gain / pan / summing

Use **4 × SSI2190** total:

- two devices contribute six channels each to the **Left** mix bus,
- two devices contribute six channels each to the **Right** mix bus.

Only eight channels per side are needed; four of the available twelve inputs per side remain unused/spare.

SSI2190 facts relevant to the design:

- six differential signal inputs into one current output,
- linear current control per channel,
- control-current range 0–100 µA,
- typical characterization on ±15 V; specified supply range ±4 V to ±18 V.

Reference: https://www.soundsemiconductor.com/downloads/ssi2190datasheet.pdf

### 6.1 Lane connection

Each SSI2140 output is buffered/split into one left-mixer input and one right-mixer input. Firmware generates two gain controls per lane.

For a mono lane, use constant-power pan mapping in firmware and calibrate the analog control path.

For a stereo logical voice occupying two lanes, normally map one lane toward left and the linked lane toward right while still permitting overall pan/width control.

### 6.2 Current-output summing

SSI2190 outputs are current-mode. Follow SSI's application/evaluation design for output compliance, transimpedance conversion, combining two devices per side, and final line-output gain.

Do not simply tie op-amp voltage outputs together. The schematic must preserve the SSI2190's intended current-output summing architecture.

## 7. Analog output stage

Required outputs:

- Main Left
- Main Right
- headphone output is optional but desirable; its amplifier is a separate design block and must not load the main summing stage.

Main output requirements:

- low-noise transimpedance/summing stage,
- DC blocking/servo decision based on measured offset,
- defined nominal level and headroom,
- output muting or controlled ramp at boot/reset,
- ESD/protection appropriate to exposed connectors,
- ground scheme compatible with chassis/jack design.

Individual voice outputs are not part of the current production requirement, but test headers can expose lanes on prototypes.

## 8. Analog rails and grounding

Initial analog IC target rails: **±12 V** is a good common system point and lies within SSI2140/SSI2190 operating ranges. Final rail choice must be validated against the selected application circuits and output headroom.

Likely board rails:

- +3.3V_A: AD1938 analog,
- +3.3V_D: AD1938 digital / DAC digital as required,
- clean DAC reference rail(s),
- +12V_A / -12V_A: SSI2140, SSI2190, analog op-amps,
- GND with deliberate analog/digital return layout.

Use one continuous ground plane where possible and control current paths by placement, not by arbitrary split planes. Keep fast SAI/SPI return currents away from filter/control/reference nodes.

The exact system DC input and ±12 V converter/regulator topology remains a power-system decision and is a fabrication blocker.

## 9. Layout strategy

Strong recommendation: **4 layers minimum; 6 layers preferred** for this mixed-signal board if size is tight.

Partition physically:

1. board connector + digital SAI/SPI,
2. AD1938 and its clean 3.3 V/reference island,
3. CV DACs/reference/conditioning,
4. eight repeated SSI2140 lanes,
5. SSI2190 left/right mixer section,
6. final output/input jack circuitry,
7. ±12 V power entry/filtering.

Keep:

- BCLK/MCLK away from VCF control nodes,
- CV SPI clocks away from codec/filter analog paths,
- DAC references and FILTR/CM loops very short,
- repeated lane geometry as symmetric as practical.

## 10. Voice stealing hardware behavior

No extra analog switching is required for ordinary voice stealing.

Firmware policy:

1. ramp SSI2190 L/R gain control to mute,
2. reassign the digital lane,
3. set new filter cutoff/Q while muted,
4. allow a short settling interval if measurements show it useful,
5. start the new amplitude ramp.

Stereo logical voices steal/release their paired lanes atomically.

## 11. Fabrication blockers / verification

1. Select the exact SSI2140 four-pole application circuit and calculate all passive values.
2. Select SSI2190 input, control-current and transimpedance circuits from the current SSI application material.
3. Define nominal audio levels at AD1938 DAC → filter → mixer → line output.
4. Define cutoff/Q and L/R gain CV ranges and design DAC scaling/buffering.
5. Decide MCP48CVB28 reference source and rail voltage.
6. Simulate/bench the sampling input stage into AD1938 differential ADCs.
7. Define the ±12 V and clean 3.3 V power tree and noise targets.
8. Verify AD1938 TDM slot order and SAI timing with the RT1176 dev carrier.
9. Build one analog lane + one L/R mixer channel as a prototype before committing eight copies.
10. Complete thermal/current budget for eight filters, four SSI2190s, codec, DACs and op-amps.

## 12. Primary references

- AD1938: https://www.analog.com/media/en/technical-documentation/data-sheets/AD1938.pdf
- MCP48CVB28 family: https://ww1.microchip.com/downloads/en/DeviceDoc/MCP48CXBX4_8-Family-Data-Sheet-20006556B.pdf
- SSI2140: https://www.soundsemiconductor.com/downloads/ssi2140datasheet.pdf
- SSI2190: https://www.soundsemiconductor.com/downloads/ssi2190datasheet.pdf
