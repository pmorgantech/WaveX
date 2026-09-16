# WaveX electrical design and review

Use this reference for circuit design, component selection, and electrical
review. Apply the sections relevant to the changed circuit; a CAD-only export
does not need a full electrical review. KiCad editing and validation procedures
remain in the [main skill](../SKILL.md) and [validation reference](validation.md).

## Contents

- [Prerequisites and evidence](#prerequisites-and-evidence)
- [Analog audio](#analog-audio)
- [CV and control](#cv-and-control)
- [Power and startup](#power-and-startup)
- [Digital interfaces](#digital-interfaces)
- [Repeated channels](#repeated-channels)
- [Related](#related)

## Prerequisites and evidence

Follow [hardware/AGENTS.md](../../../../hardware/AGENTS.md) for sources of truth,
phase alignment, and firmware reconciliation. Establish the affected parts,
nets, voltage domains, loads, and operating conditions before selecting values.
Use the exact manufacturer's part/package datasheet and module revision.
Record important assumptions and supporting calculations in the schematic or
owning hardware document; a PDF in `hardware/datasheets/` alone does not capture
a design decision.

When selecting or substituting a part, verify its exact ordering code, ratings,
package, lifecycle, and current distributor availability. Pin compatibility is
only one check. Identify an unverified part explicitly instead of inventing an
ordering code. Keep MCU pin assignments in the canonical configuration headers;
do not add a second pin table here.

## Analog audio

- Establish input/output signal range, DC bias, supply rails, and required
  headroom at each changed stage. Check input common-mode limits and output
  swing under the intended load.
- Check source/load impedance, DC offsets, coupling, and any required
  anti-aliasing or reconstruction filtering.
- For amplifier, VCF/VCA, converter, or reference substitutions, assess noise,
  distortion, bandwidth, stability, and application requirements as relevant;
  a matching pinout does not establish suitability.
- Check required decoupling and the placement/return paths of sensitive nodes
  relative to clocks and switching-power circuitry. Identify listening or bench
  measurements still needed to verify the design.

## CV and control

- Establish the destination's voltage range and load, then verify the DAC,
  reference, and any gain/offset stage can produce it across expected tolerances.
- Check resolution, settling time, capacitive-load behavior, and the required
  update rate for the complete path, including transfer and scheduling latency.
  Relate this requirement to the firmware's control-rate contract.
- Verify reset/startup outputs and the state seen by the controlled circuit
  before firmware initializes the DAC.
- If multiplexers or sample-and-hold stages are actually used, check acquisition
  time, droop over the longest hold interval, switching artifacts, and worst-case
  channel update latency. Do not introduce that topology merely because this
  checklist mentions it.

## Power and startup

- Identify each affected rail's source, consumers, voltage tolerance, noise
  requirements, and intended analog/digital/reference or high-current role.
- Estimate steady-state, peak, and startup demand. Verify regulator input range,
  output capability, thermal margin, required capacitors, and startup behavior.
- Check sequencing, reset/enable/configuration states, pull-ups/pull-downs, and
  possible back-power paths when connected domains are powered independently.
- Verify exposed-pad connections and required treatment of unused pins.
  Explain intentional rail or ground connections using the circuit's return
  paths and manufacturer guidance; rail labels alone do not determine a
  grounding strategy.

## Digital interfaces

Verify direction, voltage domains, pull-ups/termination where required, clocks,
and edge-rate constraints. Reconcile MCU pins with peripheral alternate
functions and module header routing, rather than GPIO availability alone.
Check reset states and contention during startup or partial power.

## Repeated channels

When the requested design contains replicated circuits, verify every channel's
component values, audio/control connections, connector ordering, and firmware
mapping. For example, in a requested eight-channel board, check all eight
end-to-end mappings rather than validating only the first copied channel.
Preserve intentional differences and document their purpose. Use consistent
sheet interfaces and labels; choose repeated hierarchy when it improves
maintenance without reorganizing unrelated circuitry.

## Related

- [Hardware instructions](../../../../hardware/AGENTS.md)
- [KiCad editing and automation](../SKILL.md)
- [Validation and manufacturing handoff](validation.md)
- [System architecture](../../../../docs/architecture.md)
- [Implementation roadmap](../../../../docs/roadmap.md)
