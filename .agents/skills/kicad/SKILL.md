---
name: kicad
description: Design, edit, automate, or review WaveX KiCad schematics, PCB layouts, symbols, footprints, and fabrication outputs. Use for hardware CAD work and KiCad CLI or Python IPC workflows.
---

# KiCad development for WaveX

Use this skill with [hardware/AGENTS.md](../../../hardware/AGENTS.md).
Keep electrical intent, native design files, and validation evidence consistent.
For circuit design, component selection, or electrical review, read the relevant
sections of [references/electrical-review.md](references/electrical-review.md).
For checking or exporting designs, read
[references/validation.md](references/validation.md).

## Contents

- [Prerequisites and tool choice](#prerequisites-and-tool-choice)
- [Model the design before editing](#model-the-design-before-editing)
- [Edit the smallest coherent part](#edit-the-smallest-coherent-part)
- [Completion and review](#completion-and-review)
- [Related](#related)

## Prerequisites and tool choice

Run hardware CAD work on the host with KiCad 10, as authorized by
`hardware/AGENTS.md`. Firmware builds/tests, pre-commit, and commits remain
container-only.
Inspect the installed KiCad version, project file headers, available libraries,
and task-specific command help. A missing tool is a validation limitation, not
permission to fabricate a result or change the file-format version.

Keep repeatable automation in `hardware/tools/` and its Python dependencies in
the ignored `hardware/.venv/`, installed from `hardware/requirements.txt`.
Use the local interpreter explicitly on the host, for example
`hardware/.venv/bin/python hardware/tools/<script>.py` from the repository root,
or `.venv/bin/python tools/<script>.py` from `hardware/`. Create the environment
on the host; do not reuse an incompatible container environment or
install project dependencies globally.

Use Ref for current API documentation when available; otherwise use Exa to find
official KiCad documentation. Consult the installed package's source/help for
version-specific signatures. Do not copy development-branch examples into a
released-version workflow without checking support.

Choose the tool by operation:

| Operation | Preferred route |
|---|---|
| ERC, DRC, netlist and visual exports | Installed `kicad-cli`; check subcommand help |
| Live PCB inspection or edits supported by IPC | Pinned `kicad-python` client and a compatible running editor |
| Interactive schematic/layout work unsupported by available APIs | Available KiCad editor control, with saved-file verification |
| Small native-file edits without suitable editor access | Structure-aware edits followed by native KiCad validation |

The pinned [Python requirements](../../../hardware/requirements.txt) install an
IPC client (`kicad-python`, imported as `kipy`), not a standalone CAD engine.
It requires a running KiCad instance with its API enabled. Confirm the connected
document's full path before mutations. Check capabilities for the installed
version; do not assume schematic editing exists because PCB editing does.
Do not introduce legacy `pcbnew` SWIG automation as though it were this client.

Connect the host client to the intended host editor's IPC endpoint; a venv
alone does not establish that connection. Keep one writer per document:
coordinate unsaved GUI state with disk edits so neither overwrites the other.
After an IPC timeout, inspect whether the change landed before retrying a
mutation. Batch changes into an undoable operation where the API supports it.

## Model the design before editing

State the relevant component instances (reference, value, exact package,
symbol and footprint), nets (endpoints and voltage domains), sheets/connectors,
power sources/loads, and mechanical constraints. Distinguish confirmed values
from assumptions. Use manufacturer datasheets for the exact part and board
revision; never infer module header numbering from MCU package numbering.

Keep these relationships explicit:

- Each intended component instance has a unique reference and stable identity;
  multi-unit symbols represent the same part.
- Schematic pin numbers map to actual footprint pads, including exposed pads,
  hidden power pins, connector orientation, and intentionally unused pins.
- Net labels, sheet interfaces, and junctions implement the intended connection;
  visual overlap alone does not establish it.
- PCB footprints remain associated with schematic instances. Update the board
  from the schematic after electrical changes and review additions/removals.
- Clearances, trace/via sizes, stackup, board outline, mounting positions, and
  connector envelopes come from design and fabrication constraints.

For firmware impact, use the checkout-local CodeGraph first as the root
instructions require. Native KiCad connectivity and manufacturer documents
remain the evidence for electrical relationships, even if CAD files are absent
from the code graph.

## Edit the smallest coherent part

Preserve UUIDs, reference designators, hierarchy paths, and unaffected placement
and routing. Allocate new identities only for new objects. Avoid global
reannotation, library refreshes, format upgrades, or serializers that rewrite
the whole project during an unrelated edit.

For direct edits, use a parser that preserves unfamiliar fields or a narrowly
bounded change to a known object. Do not regex-rewrite nested S-expressions or
invent the file grammar. Preserve embedded symbol definitions and instance
metadata. Native parsing/export plus connectivity and visual checks are required
before calling the result validated; balanced parentheses are insufficient.

Keep custom symbols/footprints and their library tables project-local, using
project-relative paths such as `${KIPRJMOD}` where supported. Preserve licenses
and record source/revision for imported parts. Do not depend on personal global
library tables or absolute workstation paths.

Set pin electrical types and no-connect markers to reflect the circuit.
A `PWR_FLAG` must represent a real power source. For custom footprints, verify
pad numbering and pin 1 against the manufacturer's package drawing, then check
courtyard, fabrication outline, solder-mask/paste openings, and exposed-pad
requirements. Keep libraries under `hardware/symbols/`, `hardware/footprints/`,
and `hardware/3dmodels/` as applicable.

For layout, establish mechanical constraints and placement before routing.
Keep decouplers near supply pins and return paths continuous; inspect digital
clock, switching-power, and sensitive analog coupling. Use the selected
manufacturer's stackup/rules rather than guessed universal trace widths or
blanket ground splits. Refill copper after relevant changes before final checks.

## Completion and review

Use the validation reference for baseline comparison, fresh reports, and visual
review. A clean ERC/DRC result does not prove audio quality, power integrity,
signal integrity, thermal margin, or mechanical fit.

Report specific references/nets/regions for findings and distinguish missing
tooling, design violations, and bench work. Apply the project's principle-based
review filter to significant changes. Add a changelog entry for visible changes;
do not bump the firmware version for ordinary CAD work.

## Related

- [KiCad 10 CLI manual](https://docs.kicad.org/10.0/en/cli/cli.html)
- [KiCad IPC API](https://dev-docs.kicad.org/en/apis-and-binding/ipc-api/)
- [Python client documentation](https://docs.kicad.org/kicad-python-main/)
- [Native file formats](https://dev-docs.kicad.org/en/file-formats/)
