# KiCad validation and output workflow

Use this reference when checking changed designs or preparing review or
manufacturing outputs. Commands below target the current project's KiCad 10
format; verify installed help before execution.

## Contents

- [Prerequisites](#prerequisites)
- [Baseline and fresh checks](#baseline-and-fresh-checks)
- [Connectivity and visual review](#connectivity-and-visual-review)
- [Manufacturing handoff when requested](#manufacturing-handoff-when-requested)
- [Related](#related)

## Prerequisites

Run from the repository root inside the supported devcontainer. Have the
matching KiCad application, symbol/footprint libraries, and the saved project
available. The Python IPC dependency does not provide `kicad-cli`.

```bash
kicad-cli version
kicad-cli sch erc --help
kicad-cli pcb drc --help
kicad-cli sch export svg --help
kicad-cli pcb export svg --help
```

If tools or libraries are missing, continue the inspection possible from source
and state which native checks remain unavailable. Do not claim old checked-in
reports as checks of the current edit.

## Baseline and fresh checks

Before editing, save reports and previews under `hardware/build/before/`.
After editing, use `hardware/build/after/`. Run each command independently and
record its exit status, so an ERC failure does not prevent collecting DRC.
The example below is the after-edit pass:

```bash
mkdir -p hardware/build/after
kicad-cli sch erc --format json --severity-all --exit-code-violations \
  --output hardware/build/after/erc.json hardware/wavex.kicad_sch
kicad-cli pcb drc --format json --severity-all --schematic-parity \
  --refill-zones --exit-code-violations \
  --output hardware/build/after/drc.json hardware/wavex.kicad_pcb
```

KiCad 10's `--exit-code-violations` returns 5 for reported violations.
Other failures need diagnosis; exit 0 is meaningful only with the intended
input and checks. `--severity-all` includes exclusions; inspect ignored check
settings too. Distinguish inherited exclusions from new ones and explain any
intentional rule change. Do not relax severities or clear exclusions just to
change the reported count.

`--refill-zones` checks refilled copper; without `--save-board`, this command
does not persist that refill. Save final fills deliberately in the editor or
with the supported save option, inspect the resulting diff, and validate that
saved state before fabrication exports.

Compare findings by rule, affected reference/net, and location, not just count
or report timestamp. Confirm that expected components and nets actually exist.
An empty schematic can pass ERC; an unrouted or empty board is not finished.

## Connectivity and visual review

For electrical changes, export a netlist with `kicad-cli sch export netlist`
after checking its help. Compare intended pin-to-net membership before and
after; verify changed connectors and supply domains against canonical
configuration and exact datasheets.

Generate review images:

```bash
kicad-cli sch export svg --output hardware/build/after/schematic/ \
  hardware/wavex.kicad_sch
kicad-cli pcb export svg --layers F.Cu,B.Cu,F.SilkS,B.SilkS,Edge.Cuts \
  --output hardware/build/after/pcb/ hardware/wavex.kicad_pcb
```

Adjust the layer list for the actual board, including inner copper and mask
when relevant. Inspect every affected sheet/layer using an available renderer
or editor. If no visual tool is available, report that limitation explicitly.
Check label/pin readability, junctions, connector orientation, polarity,
silkscreen, board outline closure, mounting clearances, and placement against
mechanical constraints. Inspect 3D/mechanical views when enclosure fit matters.

## Manufacturing handoff when requested

Use the installed `pcb export` help for Gerbers, drill files and placement
outputs, and `sch export` help for BOM output. Establish layers, units,
coordinate origin, bottom-side conventions, and assembly variants with the
selected fabricator's requirements. Do not invent a manufacturer profile.

Export from the same saved and checked revision. Inspect Gerber/drill alignment,
outline and cutouts in a viewer. Reconcile BOM and placement references, DNP
parts, quantities, exact ordering codes, footprint packages, sides, and
rotations. Keep outputs together under a dedicated `hardware/build/` directory
and identify the source revision and tool version; include dirty-state changes
in that identification when applicable.

Creating an output package does not itself establish fabrication readiness.
List unresolved violations and unverified electrical or mechanical assumptions
with the handoff. Ordering boards is a separate action from generating files.

## Related

- [KiCad skill](../SKILL.md)
- [Hardware instructions](../../../../hardware/AGENTS.md)
- [KiCad 10 CLI options](https://docs.kicad.org/10.0/en/cli/cli.html)
